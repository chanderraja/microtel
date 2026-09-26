# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# leaf_mqtt: a C leaf publishing over MQTT with coreMQTT, and a C++
# concentrator subscribing with libmosquitto. Both halves or neither: the
# example needs -DMICROTEL_BUILD_LEAF=ON, -DMICROTEL_WITH_CONCENTRATOR=ON and
# libmosquitto's development files (Fedora: mosquitto-devel; Debian/Ubuntu:
# libmosquitto-dev). Without any of them it is skipped with a message.
#
# Both MQTT libraries belong to the example alone. coreMQTT is fetched here and
# compiled into the leaf binary; libmosquitto is a system library linked into
# the concentrator binary. Neither is linked into any microtel library, so
# microtel's runtime closure (CLAUDE.md rule 12) is unchanged.

if(NOT TARGET microtel_leaf OR NOT MICROTEL_WITH_CONCENTRATOR)
    message(STATUS "examples/leaf_mqtt: skipped (needs -DMICROTEL_BUILD_LEAF=ON "
                   "and -DMICROTEL_WITH_CONCENTRATOR=ON)")
    return()
endif()

find_path(MICROTEL_MOSQUITTO_INCLUDE_DIR mosquitto.h)
find_library(MICROTEL_MOSQUITTO_LIBRARY mosquitto)
if(NOT MICROTEL_MOSQUITTO_INCLUDE_DIR OR NOT MICROTEL_MOSQUITTO_LIBRARY)
    message(STATUS "examples/leaf_mqtt: skipped: libmosquitto not found. Install "
                   "mosquitto-devel (Fedora) or libmosquitto-dev (Debian/Ubuntu), "
                   "or point CMAKE_PREFIX_PATH at it.")
    return()
endif()

# coreMQTT v2.3.1 is the last release of the MQTT 3.1.1 client; the v5 line
# speaks MQTT 5. The repository has no CMakeLists.txt of its own, so
# FetchContent only downloads it, and mqttFilePaths.cmake lists the sources.
include(FetchContent)
FetchContent_Declare(
    coremqtt
    GIT_REPOSITORY https://github.com/FreeRTOS/coreMQTT.git
    GIT_TAG        v2.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(coremqtt)
include(${coremqtt_SOURCE_DIR}/mqttFilePaths.cmake)

# Third-party C, built as it ships: no -Werror, and its headers are SYSTEM so
# they cannot trip the leaf's warnings. MQTT_DO_NOT_USE_CUSTOM_CONFIG takes
# the library's defaults (core_mqtt_config_defaults.h) instead of asking for a
# core_mqtt_config.h: logging off, 10 ms receive polling.
add_library(microtel_example_coremqtt STATIC ${MQTT_SOURCES} ${MQTT_SERIALIZER_SOURCES})
target_include_directories(microtel_example_coremqtt SYSTEM PUBLIC ${MQTT_INCLUDE_PUBLIC_DIRS})
target_compile_definitions(microtel_example_coremqtt PUBLIC MQTT_DO_NOT_USE_CUSTOM_CONFIG)
set_target_properties(microtel_example_coremqtt PROPERTIES C_STANDARD 11)

add_executable(microtel_example_leaf_mqtt_leaf mqtt_leaf.c)
target_link_libraries(microtel_example_leaf_mqtt_leaf PRIVATE
    microtel_leaf microtel_example_coremqtt)
set_target_properties(microtel_example_leaf_mqtt_leaf PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
target_compile_options(microtel_example_leaf_mqtt_leaf PRIVATE -Wall -Wextra -Wpedantic -Werror)

microtel_add_example(leaf_mqtt_concentrator SOURCES mqtt_concentrator.cpp)
target_include_directories(microtel_example_leaf_mqtt_concentrator SYSTEM PRIVATE
    ${MICROTEL_MOSQUITTO_INCLUDE_DIR})
target_link_libraries(microtel_example_leaf_mqtt_concentrator PRIVATE
    ${MICROTEL_MOSQUITTO_LIBRARY})
target_compile_definitions(microtel_example_leaf_mqtt_concentrator PRIVATE
    MICROTEL_EXAMPLE_LEAF_MQTT_TOML="${CMAKE_CURRENT_SOURCE_DIR}/microtel.toml")
