# The cue player (spec 9).  wav.cpp is pure and host-tested; player.cpp is the
# I2S/LittleFS shell and contains no policy.
idf_component_register(SRCS "wav.cpp" "player.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES swan_hal esp_driver_i2s esp_timer)
