FROM jaegertracing/all-in-one@sha256:f307b1ce9bd9f85462e63384ae69d44ecdcbd264ec6811c879eb6a50a440024f
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
