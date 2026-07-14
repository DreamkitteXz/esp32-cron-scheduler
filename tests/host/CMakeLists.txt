cmake_minimum_required(VERSION 3.16)
project(cron_host_tests C)

# Compila SÓ components/cron. É possível porque o componente é C puro (sem esp_*, freertos/*
# ou driver/*), então o parser CRON roda e é testado no host, sem hardware e sem ESP-IDF.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CRON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/cron)

include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.0
)
FetchContent_MakeAvailable(unity)

# Habilita os asserts de 64 bits (TEST_ASSERT_EQUAL_HEX64) na própria unity.c, não só nos testes.
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_64)

add_executable(test_cron test_cron.c ${CRON_DIR}/src/cron.c)
target_include_directories(test_cron PRIVATE ${CRON_DIR}/include)
target_link_libraries(test_cron PRIVATE unity)

if(NOT MSVC)
    target_compile_options(test_cron PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME cron COMMAND test_cron)
