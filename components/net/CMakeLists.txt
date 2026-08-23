# WiFi STA + mDNS + esp_http_server (spec 10.1/10.2), plus the IDF adapters for
# webapi's interfaces.  Transport only: every command goes through webapi's
# single dispatcher, and nothing here runs on the motion path.
idf_component_register(SRCS "wifi.cpp" "httpd.cpp" "bindings.cpp" "mqtt.cpp" "ota.cpp" "provision.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES webapi config ring modes motion frame swan_hal
                                esp_wifi esp_netif esp_event esp_http_server
                                esp_timer nvs_flash esp_app_format mqtt app_update esp_partition lwip)
# `mqtt` is esp-mqtt, an IN-TREE IDF component - not a managed one, so
# CLAUDE.md's rule about recording a reason in README.md does not apply.
# Noted here so nobody reaches for a managed component later by reflex.
