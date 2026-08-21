# Pure logic - no IDF dependencies, so test/host can compile these sources too.
idf_component_register(SRCS "ring.cpp"
                       INCLUDE_DIRS "include")
