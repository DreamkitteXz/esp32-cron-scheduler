idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES cli clk led net sched
                       PRIV_REQUIRES nvs_flash)
