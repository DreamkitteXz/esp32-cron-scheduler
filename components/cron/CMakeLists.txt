# Componente C puro: sem REQUIRES do ESP-IDF de propósito.
# O mesmo src/cron.c é compilado pelo CMake de host em tests/host/ (ver README).
idf_component_register(SRCS "src/cron.c"
                       INCLUDE_DIRS "include")
