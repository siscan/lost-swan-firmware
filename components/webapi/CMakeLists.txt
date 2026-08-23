# The web API, pure (spec 10.2 / 10.2a): state payload, command dispatcher and
# the ring-upload stager.  No IDF includes, so the host dev server, the host
# tests and esp_http_server all run the same code; the HTTP transport itself
# lives in components/net.
idf_component_register(SRCS "api.cpp" "ring_upload.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES modes ring motion frame swan_hal)
