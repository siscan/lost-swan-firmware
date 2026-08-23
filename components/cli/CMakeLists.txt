idf_component_register(SRCS "cli.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES motion config ring modes frame net swan_hal console esp_driver_usb_serial_jtag)
