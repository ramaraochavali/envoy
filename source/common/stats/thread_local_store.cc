#include "common/stats/thread_local_store.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Envoy {
namespace Stats {

ThreadLocalStoreImpl::ThreadLocalStoreImpl(RawStatDataAllocator& alloc)
    : alloc_(alloc), default_scope_(createScope("")),
      tag_producer_(std::make_unique<TagProducerImpl>()),
      num_last_resort_stats_(default_scope_->counter("stats.overflow")) {}

ThreadLocalStoreImpl::~ThreadLocalStoreImpl() {
  ASSERT(shutting_down_);
  default_scope_.reset();
  ASSERT(scopes_.empty());
}

std::list<CounterSharedPtr> ThreadLocalStoreImpl::counters() const {
  // Handle de-dup due to overlapping scopes.
  std::list<CounterSharedPtr> ret;
  std::unordered_set<std::string> names;
  std::unique_lock<std::mutex> lock(lock_);
  for (ScopeImpl* scope : scopes_) {
    for (auto counter : scope->central_cache_.counters_) {
      if (names.insert(counter.first).second) {
        ret.push_back(counter.second);
      }
    }
  }

  return ret;
}

ScopePtr ThreadLocalStoreImpl::createScope(const std::string& name) {
  std::unique_ptr<ScopeImpl> new_scope(new ScopeImpl(*this, name));
  std::unique_lock<std::mutex> lock(lock_);
  scopes_.emplace(new_scope.get());
  return std::move(new_scope);
}

std::list<GaugeSharedPtr> ThreadLocalStoreImpl::gauges() const {
  // Handle de-dup due to overlapping scopes.
  std::list<GaugeSharedPtr> ret;
  std::unordered_set<std::string> names;
  std::unique_lock<std::mutex> lock(lock_);
  for (ScopeImpl* scope : scopes_) {
    for (auto gauge : scope->central_cache_.gauges_) {
      if (names.insert(gauge.first).second) {
        ret.push_back(gauge.second);
      }
    }
  }

  return ret;
}

std::list<HistogramSharedPtr> ThreadLocalStoreImpl::histograms() const {
  // Handle de-dup due to overlapping scopes.
  std::list<HistogramSharedPtr> ret;
  std::unordered_set<std::string> names;
  std::unique_lock<std::mutex> lock(lock_);
  // TODO(ramaraochavali): incorporate the scopes into the histogram names.
  for (ScopeImpl* scope : scopes_) {
    for (auto histogram : scope->central_cache_.histograms_) {
      const std::string& hist_name = histogram.first;
      const ParentHistogramSharedPtr& parent_hist = histogram.second;
      if (names.insert(hist_name).second) {
        ret.push_back(parent_hist);
      }
    }
  }

  return ret;
}

void ThreadLocalStoreImpl::initializeThreading(Event::Dispatcher& main_thread_dispatcher,
                                               ThreadLocal::Instance& tls) {
  main_thread_dispatcher_ = &main_thread_dispatcher;
  tls_ = tls.allocateSlot();
  tls_->set([](Event::Dispatcher&) -> ThreadLocal::ThreadLocalObjectSharedPtr {
    return std::make_shared<TlsCache>();
  });
}

void ThreadLocalStoreImpl::shutdownThreading() {
  // This will block both future cache fills as well as cache flushes.
  shutting_down_ = true;
}

void ThreadLocalStoreImpl::mergeHistograms(PostMergeCb merge_complete_cb) {
  if (!shutting_down_) {
    tls_->runOnAllThreadsWithBarrier(
        [this]() -> void {
          for (ScopeImpl* scope : scopes_) {
            for (auto histogram : tls_->getTyped<TlsCache>().scope_cache_[scope].histograms_) {
              const TlsHistogramSharedPtr& tls_hist = histogram.second;
              tls_hist->beginMerge();
            }
          }
        },
        [this, merge_complete_cb]() -> void { mergeInternal(merge_complete_cb); });
  }
}

void ThreadLocalStoreImpl::mergeInternal(PostMergeCb merge_complete_cb) {
  if (!shutting_down_) {
    for (HistogramSharedPtr histogram : histograms()) {
      histogram->merge();
    }
    merge_complete_cb();
  }
}

void ThreadLocalStoreImpl::releaseScopeCrossThread(ScopeImpl* scope) {
  std::unique_lock<std::mutex> lock(lock_);
  ASSERT(scopes_.count(scope) == 1);
  scopes_.erase(scope);

  // This can happen from any thread. We post() back to the main thread which will initiate the
  // cache flush operation.
  if (!shutting_down_ && main_thread_dispatcher_) {
    main_thread_dispatcher_->post([this, scope]() -> void { clearScopeFromCaches(scope); });
  }
}

std::string ThreadLocalStoreImpl::getTagsForName(const std::string& name, std::vector<Tag>& tags) {
  return tag_producer_->produceTags(name, tags);
}

void ThreadLocalStoreImpl::clearScopeFromCaches(ScopeImpl* scope) {
  // If we are shutting down we no longer perform cache flushes as workers may be shutting down
  // at the same time.
  if (!shutting_down_) {
    // Perform a cache flush on all threads.
    tls_->runOnAllThreads(
        [this, scope]() -> void { tls_->getTyped<TlsCache>().scope_cache_.erase(scope); });
  }
}

ThreadLocalStoreImpl::SafeAllocData ThreadLocalStoreImpl::safeAlloc(const std::string& name) {
  RawStatData* data = alloc_.alloc(name);
  if (!data) {
    // If we run out of stat space from the allocator (which can happen if for example allocations
    // are coming from a fixed shared memory region, we need to deal with this case the best we
    // can. We must pass back the right allocator so that free() happens on the heap.
    num_last_resort_stats_.inc();
    return {*heap_allocator_.alloc(name), heap_allocator_};
  } else {
    return {*data, alloc_};
  }
}

ThreadLocalStoreImpl::ScopeImpl::~ScopeImpl() { parent_.releaseScopeCrossThread(this); }

Counter& ThreadLocalStoreImpl::ScopeImpl::counter(const std::string& name) {
  // Determine the final name based on the prefix and the passed name.
  std::string final_name = prefix_ + name;

  // We now try to acquire a *reference* to the TLS cache shared pointer. This might remain null
  // if we don't have TLS initialized currently. The de-referenced pointer might be null if there
  // is no cache entry.
  CounterSharedPtr* tls_ref = nullptr;
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_ref = &parent_.tls_->getTyped<TlsCache>().scope_cache_[this].counters_[final_name];
  }

  // If we have a valid cache entry, return it.
  if (tls_ref && *tls_ref) {
    return **tls_ref;
  }

  // We must now look in the central store so we must be locked. We grab a reference to the
  // central store location. It might contain nothing. In this case, we allocate a new stat.
  std::unique_lock<std::mutex> lock(parent_.lock_);
  CounterSharedPtr& central_ref = central_cache_.counters_[final_name];
  if (!central_ref) {
    SafeAllocData alloc = parent_.safeAlloc(final_name);
    std::vector<Tag> tags;
    std::string tag_extracted_name = parent_.getTagsForName(final_name, tags);
    central_ref.reset(
        new CounterImpl(alloc.data_, alloc.free_, std::move(tag_extracted_name), std::move(tags)));
  }

  // If we have a TLS location to store or allocation into, do it.
  if (tls_ref) {
    *tls_ref = central_ref;
  }

  // Finally we return the reference.
  return *central_ref;
}

void ThreadLocalStoreImpl::ScopeImpl::deliverHistogramToSinks(const Histogram& histogram,
                                                              uint64_t value) {
  // Thread local deliveries must be blocked outright for histograms and timers during shutdown.
  // This is because the sinks may end up trying to create new connections via the thread local
  // cluster manager which may already be destroyed (there is no way to sequence this because the
  // cluster manager destroying can create deliveries). We special case this explicitly to avoid
  // having to implement a shutdown() method (or similar) on every TLS object.
  if (parent_.shutting_down_) {
    return;
  }

  for (Sink& sink : parent_.timer_sinks_) {
    sink.onHistogramComplete(histogram, value);
  }
}

Gauge& ThreadLocalStoreImpl::ScopeImpl::gauge(const std::string& name) {
  // See comments in counter(). There is no super clean way (via templates or otherwise) to
  // share this code so I'm leaving it largely duplicated for now.
  std::string final_name = prefix_ + name;
  GaugeSharedPtr* tls_ref = nullptr;
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_ref = &parent_.tls_->getTyped<TlsCache>().scope_cache_[this].gauges_[final_name];
  }

  if (tls_ref && *tls_ref) {
    return **tls_ref;
  }

  std::unique_lock<std::mutex> lock(parent_.lock_);
  GaugeSharedPtr& central_ref = central_cache_.gauges_[final_name];
  if (!central_ref) {
    SafeAllocData alloc = parent_.safeAlloc(final_name);
    std::vector<Tag> tags;
    std::string tag_extracted_name = parent_.getTagsForName(final_name, tags);
    central_ref.reset(
        new GaugeImpl(alloc.data_, alloc.free_, std::move(tag_extracted_name), std::move(tags)));
  }

  if (tls_ref) {
    *tls_ref = central_ref;
  }

  return *central_ref;
}

Histogram& ThreadLocalStoreImpl::ScopeImpl::histogram(const std::string& name) {
  // See comments in counter(). There is no super clean way (via templates or otherwise) to
  // share this code so I'm leaving it largely duplicated for now.
  std::string final_name = prefix_ + name;
  TlsHistogramSharedPtr* tls_ref = nullptr;
  if (!parent_.shutting_down_ && parent_.tls_) {
    tls_ref = &parent_.tls_->getTyped<TlsCache>().scope_cache_[this].histograms_[final_name];
  }

  if (tls_ref && *tls_ref) {
    return **tls_ref;
  }

  std::unique_lock<std::mutex> lock(parent_.lock_);
  ParentHistogramSharedPtr& central_ref = central_cache_.histograms_[final_name];

  std::vector<Tag> tags;
  std::string tag_extracted_name = parent_.getTagsForName(final_name, tags);
  if (!central_ref) {
    // Since MetricImpl only has move constructor, we are explicitly copying here.
    std::string central_tag_extracted_name(tag_extracted_name);
    std::vector<Tag> central_tags(tags);
    central_ref.reset(new HistogramParentImpl(
        final_name, parent_, std::move(central_tag_extracted_name), std::move(central_tags)));
  }
  TlsHistogramSharedPtr hist_tls_ptr(new ThreadLocalHistogramImpl(
      final_name, parent_, std::move(tag_extracted_name), std::move(tags)));
  central_ref->addTlsHistogram(hist_tls_ptr);

  if (tls_ref) {
    *tls_ref = hist_tls_ptr;
  }
  return *hist_tls_ptr;
}

ThreadLocalHistogramImpl::ThreadLocalHistogramImpl(const std::string& name, Store& parent,
                                                   std::string&& tag_extracted_name,
                                                   std::vector<Tag>&& tags)
    : MetricImpl(name, std::move(tag_extracted_name), std::move(tags)), parent_(parent),
      current_active_(0), flags_(0) {
  histograms_[0] = hist_alloc();
  histograms_[1] = hist_alloc();
}

ThreadLocalHistogramImpl::~ThreadLocalHistogramImpl() {
  hist_free(histograms_[0]);
  hist_free(histograms_[1]);
}

void ThreadLocalHistogramImpl::recordValue(uint64_t value) {
  hist_insert_intscale(histograms_[current_active_], value, 0, 1);
  parent_.deliverHistogramToSinks(*this, value);
  flags_ |= Flags::Used;
}

void ThreadLocalHistogramImpl::merge(histogram_t* target) {
  histogram_t* hist_array[1];
  hist_array[0] = histograms_[1 - current_active_];
  hist_accumulate(target, hist_array, ARRAY_SIZE(hist_array));
  hist_clear(hist_array[0]);
}

HistogramParentImpl::HistogramParentImpl(const std::string& name, Store& parent,
                                         std::string&& tag_extracted_name, std::vector<Tag>&& tags)
    : MetricImpl(name, std::move(tag_extracted_name), std::move(tags)), parent_(parent),
      interval_histogram_(hist_alloc()), cumulative_histogram_(hist_alloc()),
      interval_statistics_(interval_histogram_), cumulative_statistics_(cumulative_histogram_) {}

bool HistogramParentImpl::used() const {
  std::unique_lock<std::mutex> lock(merge_lock_);
  return usedWorker();
}

HistogramParentImpl::~HistogramParentImpl() {
  hist_free(interval_histogram_);
  hist_free(cumulative_histogram_);
}

/**
 * This method is called during the main stats flush process for each of the histogram. This
 * method iterates through the Tls histograms and collects the histogram data of all of them
 * in to "interval_histogram_". Then the collected "interval_histogram_" is merged to a
 * "cumulative_histogram". More details about threading model at
 * https://github.com/envoyproxy/envoy/issues/1965#issuecomment-376672282.
 */
void HistogramParentImpl::merge() {
  std::unique_lock<std::mutex> lock(merge_lock_);
  if (usedWorker()) {
    hist_clear(interval_histogram_);
    for (TlsHistogramSharedPtr tls_histogram : tls_histograms_) {
      tls_histogram->merge(interval_histogram_);
    }
    histogram_t* hist_array[1];
    hist_array[0] = interval_histogram_;
    hist_accumulate(cumulative_histogram_, hist_array, ARRAY_SIZE(hist_array));
    cumulative_statistics_.refresh(cumulative_histogram_);
    interval_statistics_.refresh(interval_histogram_);
  }
}

void HistogramParentImpl::addTlsHistogram(TlsHistogramSharedPtr hist_ptr) {
  std::unique_lock<std::mutex> lock(merge_lock_);
  tls_histograms_.emplace_back(hist_ptr);
}

bool HistogramParentImpl::usedWorker() const {
  bool any_tls_used = false;
  for (const TlsHistogramSharedPtr tls_histogram : tls_histograms_) {
    if (tls_histogram->used()) {
      any_tls_used = true;
      break;
    }
  }
  return any_tls_used;
}

} // namespace Stats
} // namespace Envoy
