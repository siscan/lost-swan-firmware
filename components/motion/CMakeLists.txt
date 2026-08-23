idf_component_register(SRCS "motion.cpp" "axis_control.cpp" "fault_policy.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES swan_hal ring esp_driver_gptimer esp_driver_gpio)
