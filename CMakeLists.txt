cmake_minimum_required(VERSION 3.16)
project(rsreticulum_sx1262 VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod<2)
pkg_check_modules(CYAML REQUIRED IMPORTED_TARGET libcyaml)
find_package(Threads REQUIRED)

set(CONFIG_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/sx1262.schema.json")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_SCHEMA "${GENERATED_DIR}/schema.h")
add_custom_command(
    OUTPUT "${GENERATED_SCHEMA}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=${CONFIG_SCHEMA}"
        "-DOUTPUT=${GENERATED_SCHEMA}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS "${CONFIG_SCHEMA}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    VERBATIM)

add_library(sx1262 MODULE
    "${GENERATED_SCHEMA}"
    src/config.c
    src/plugin.c
    src/protocol.c
    src/sx126x.c)
set_target_properties(sx1262 PROPERTIES PREFIX "" OUTPUT_NAME "sx1262")
target_compile_features(sx1262 PRIVATE c_std_11)
target_compile_definitions(sx1262 PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(sx1262 PRIVATE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
target_include_directories(sx1262 PRIVATE "${GENERATED_DIR}" include src)
target_link_libraries(sx1262 PRIVATE PkgConfig::GPIOD PkgConfig::CYAML Threads::Threads m)

install(TARGETS sx1262 LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/reticulum-rs")

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol_test tests/protocol_test.c src/protocol.c)
    target_compile_features(protocol_test PRIVATE c_std_11)
    target_compile_options(protocol_test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_include_directories(protocol_test PRIVATE src)
    add_test(NAME protocol COMMAND protocol_test)

    add_executable(config_test tests/config_test.c src/config.c)
    target_compile_features(config_test PRIVATE c_std_11)
    target_compile_options(config_test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_include_directories(config_test PRIVATE src)
    target_link_libraries(config_test PRIVATE PkgConfig::CYAML)
    add_test(NAME config COMMAND config_test)

    add_executable(api_test tests/api_test.c)
    target_compile_features(api_test PRIVATE c_std_11)
    target_compile_options(api_test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    target_include_directories(api_test PRIVATE include)
    target_link_libraries(api_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME api COMMAND api_test $<TARGET_FILE:sx1262>)
endif()
