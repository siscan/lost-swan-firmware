# WiFi STA + mDNS + esp_http_server (spec 10.1/10.2), plus the IDF adapters for
# webapi's interfaces.  Transport only: every command goes through webapi's
# single dispatcher, and nothing here runs on the motion path.
idf_component_register(SRCS "wifi.cpp" "httpd.cpp" "bindings.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES webapi config ring modes motion frame swan_hal
                                esp_wifi esp_netif esp_event esp_http_server
                                esp_timer nvs_flash esp_app_format)
