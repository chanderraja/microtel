# M14 L6 — spdlog bridge sink.
microtel_add_test(
    NAME    spdlog_sink_test
    SOURCES spdlog_sink_test.cpp
    LIBS    microtel_spdlog_bridge)
target_include_directories(spdlog_sink_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(spdlog_sink_test PRIVATE
    -Wno-missing-field-initializers)
