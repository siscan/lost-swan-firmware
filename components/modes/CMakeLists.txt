# Entirely pure - clock, message, countdown and arbitration are host-tested;
# the wiring to real time / motion / NVS lives in main and the shells.
idf_component_register(SRCS "render.cpp" "mode_manager.cpp" "wear.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES frame ring timesvc swan_hal journal)
