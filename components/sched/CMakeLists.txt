idf_component_register(SRCS "src/sched.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common cron
                       PRIV_REQUIRES led)
