# ring_runtime.cpp / json_lite.cpp are pure (host-compiled by the test suite);
# ring_store.cpp is the IDF shell (LittleFS).
idf_component_register(SRCS "ring_runtime.cpp" "json_lite.cpp" "ring_store.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES swan_hal)
