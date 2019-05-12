#pragma once

#include <functional>
#include <map>
#include <memory>

#include "envoy/admin/v2alpha/config_dump.pb.h"
#include "envoy/common/pure.h"

#include "common/common/non_copyable.h"
#include "common/protobuf/protobuf.h"

namespace Envoy {
namespace Server {

/**
 * ConfigTracker is used by the `/config_dump` admin endpoint to manage storage of config-providing
 * callbacks with weak ownership semantics. Callbacks added to ConfigTracker only live as long as
 * the returned EntryOwner object (or ConfigTracker itself, if shorter). Keys should be descriptors
 * of the configs provided by the corresponding callback. They must be unique.
 * ConfigTracker is *not* threadsafe.
 */
class ConfigTracker {
public:
  typedef std::function<ProtobufTypes::MessagePtr()> Cb;
  typedef std::map<std::string, Cb> CbsMap;
  typedef std::shared_ptr<envoy::admin::v2alpha::ControlPlaneConfigDump> ControlPlaneConfigPtr;
  typedef std::map<std::string, ControlPlaneConfigPtr> ControlPlaneConfigMap;

  /**
   * EntryOwner supplies RAII semantics for entries in the map.
   * The entry is not removed until the EntryOwner or the ConfigTracker itself is destroyed,
   * whichever happens first. When you add() an entry, you must hold onto the returned
   * owner object for as long as you want the entry to stay in the map.
   */
  class EntryOwner {
  public:
    virtual ~EntryOwner() {}

  protected:
    EntryOwner(){}; // A sly way to make this class "abstract."
  };
  typedef std::unique_ptr<EntryOwner> EntryOwnerPtr;

  virtual ~ConfigTracker(){};

  /**
   * @return const CbsMap& The map of string keys to tracked callbacks.
   */
  virtual const CbsMap& getCallbacksMap() const PURE;

  /**
   * Add a new callback to the map under the given key
   * @param key the map key for the new callback.
   * @param cb the callback to add. *must not* return nullptr.
   * @return EntryOwnerPtr the new entry's owner object. nullptr if the key is already present.
   */
  virtual EntryOwnerPtr add(const std::string& key, Cb cb) PURE;

  /**
   * Add or update a managed config to the config tracker under the given key
   * @param xds_service the service for which the configuration is being tracked.
   * @param control_plane_config the message to be managed by config tracker.
   */
  virtual void addOrUpdateControlPlaneConfig(const std::string& xds_service,
                                             ControlPlaneConfigPtr control_plane_config) PURE;

  /**
   * Returns config managed by config tracker under the given service
   * @param xds_service the key for the configuration.
   * @return ControlPlaneConfigPtr the config tracked for the service.
   */
  virtual ControlPlaneConfigPtr getControlPlaneConfig(const std::string& xds_service) const PURE;

  /**
   * Returns control plane config map by config tracker.
   * @return ControlPlaneConfigMap the message map managed by config tracker.
   */
  virtual const ControlPlaneConfigMap& getControlPlaneConfigMap() const PURE;
};

} // namespace Server
} // namespace Envoy
