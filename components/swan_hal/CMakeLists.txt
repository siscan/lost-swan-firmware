# Named swan_hal, not hal: ESP-IDF already ships a first-party component called
# "hal", and a project component of the same name shadows it and breaks the
# build.  Headers keep the hal/ prefix, so includes read as CLAUDE.md intends.
idf_component_register(SRCS "status_led.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio)
