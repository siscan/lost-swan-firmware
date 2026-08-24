idf_component_register(SRCS "cli.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES audio motion config ring modes frame net webapi swan_hal console esp_driver_usb_serial_jtag)
