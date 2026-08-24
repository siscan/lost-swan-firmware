# log_ring.cpp / journal_record.cpp are pure (host-compiled by the test suite);
# journal.cpp is the IDF shell - the queue, the writer task and the file.
idf_component_register(SRCS "log_ring.cpp" "journal_record.cpp" "journal.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ring esp_timer)
