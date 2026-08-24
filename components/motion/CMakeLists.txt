idf_component_register(SRCS "motion.cpp" "axis_control.cpp" "fault_policy.cpp" "soak.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES swan_hal ring journal esp_driver_gptimer esp_driver_gpio)
