idf_component_register(SRCS "pn532.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver freertos
                       PRIV_REQUIRES board)
