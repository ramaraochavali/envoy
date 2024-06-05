FROM jaegertracing/all-in-one@sha256:9b4afd8c211b182bc8a83ee55c42e9a41dab058d1f9c2fd76d4f7e5f43f4bf4a
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
