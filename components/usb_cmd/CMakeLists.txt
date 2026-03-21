idf_component_register(SRCS "usb_cmd.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_driver_usb_serial_jtag console)