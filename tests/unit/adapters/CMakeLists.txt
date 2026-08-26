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

# M17 L1 — pins the opentelemetry-cpp API configuration the shim is built
# against. Mostly static_asserts: the failure this guards against (falling back
# to otel-cpp's vendored abseil snapshot) is invisible at runtime, so it has to
# be caught at compile time.
if(MICROTEL_BUILD_OTELCPP_SHIM)
    microtel_add_test(
        NAME    otelcpp_config_test
        SOURCES otelcpp_config_test.cpp
        LIBS    microtel_otelcpp_shim)
endif()
