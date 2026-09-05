idf_component_register(SRCS "scanner.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb espressif__usb_host_hid freertos)