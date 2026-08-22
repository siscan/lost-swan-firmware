# tz.cpp is pure (host-compiled by the tests); time_service.cpp is the IDF
# shell (esp_sntp).  SNTP needs esp_netif; WiFi arrives in Phase 4.
idf_component_register(SRCS "tz.cpp" "time_service.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif lwip)
