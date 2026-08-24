idf_component_register(
    SRCS "gesture.cpp" "button.cpp"
    INCLUDE_DIRS "include"
    REQUIRES swan_hal webapi modes driver esp_timer)
