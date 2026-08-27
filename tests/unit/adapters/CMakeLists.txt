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

    # M17 L2 — otel-cpp's 16-alternative AttributeValue to microtel's 8.
    microtel_add_test(
        NAME    otelcpp_attribute_conversion_test
        SOURCES otelcpp_attribute_conversion_test.cpp
        LIBS    microtel_otelcpp_shim)
    target_include_directories(otelcpp_attribute_conversion_test
        PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # M17 L2 — TraceId / SpanId / SpanContext bridging, both directions.
    microtel_add_test(
        NAME    otelcpp_context_conversion_test
        SOURCES otelcpp_context_conversion_test.cpp
        LIBS    microtel_otelcpp_shim)
    target_include_directories(otelcpp_context_conversion_test
        PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # M17 L2 — SpanShim: otel-cpp trace::Span over a microtel SpanHandle.
    # microtel_sdk provides the out-of-line SpanDeleter::operator() the
    # SpanHandle in the test destroys; consumers link it as a matter of course.
    microtel_add_test(
        NAME    otelcpp_span_shim_test
        SOURCES otelcpp_span_shim_test.cpp
        LIBS    microtel_otelcpp_shim microtel_sdk)
    target_include_directories(otelcpp_span_shim_test
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests)

    # M17 L2 — TracerShim / TracerProviderShim over microtel Tracer/Provider.
    microtel_add_test(
        NAME    otelcpp_tracer_shim_test
        SOURCES otelcpp_tracer_shim_test.cpp
        LIBS    microtel_otelcpp_shim microtel_sdk)
    target_include_directories(otelcpp_tracer_shim_test
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests)

    # M17 L3 — MeterShim / MeterProviderShim over microtel Meter/Provider.
    microtel_add_test(
        NAME    otelcpp_meter_shim_test
        SOURCES otelcpp_meter_shim_test.cpp
        LIBS    microtel_otelcpp_shim microtel_sdk)
    target_include_directories(otelcpp_meter_shim_test
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests)
endif()
