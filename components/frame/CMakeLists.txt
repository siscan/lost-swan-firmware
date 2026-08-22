# frame.cpp is pure (host-compiled by the tests); motion_port.cpp is the IDF
# adapter onto motion::.
idf_component_register(SRCS "frame.cpp" "motion_port.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES motion ring swan_hal)
