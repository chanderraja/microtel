cmake_minimum_required(VERSION 3.28)
project(emit_app CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Backend selection — passed by the SUT Dockerfile via -DBENCH_BACKEND=<name>.
# MICROTEL     : microtel SDK, OTLP/HTTP-protobuf
# OTELCPP_GRPC : opentelemetry-cpp SDK, OTLP/gRPC
# OTELCPP_HTTP : opentelemetry-cpp SDK, OTLP/HTTP-protobuf  (B1+)
set(BENCH_BACKEND "MICROTEL" CACHE STRING
    "Emit-app backend: MICROTEL | OTELCPP_GRPC | OTELCPP_HTTP")
set_property(CACHE BENCH_BACKEND PROPERTY
    STRINGS MICROTEL OTELCPP_GRPC OTELCPP_HTTP)

# Inherit compiler flags from the base Dockerfile environment variable.
# On a developer machine without the variable set, falls back to a sensible default.
if(DEFINED ENV{BENCH_CXXFLAGS})
    string(REPLACE " " ";" _bench_flags "$ENV{BENCH_CXXFLAGS}")
    add_compile_options(${_bench_flags})
else()
    add_compile_options(-O2 -g -fno-omit-frame-pointer)
endif()

add_executable(emit_app
    src/main.cpp
    src/histogram.cpp
    src/control_socket.cpp
)

target_compile_definitions(emit_app PRIVATE
    BENCH_BACKEND_${BENCH_BACKEND}=1)

target_include_directories(emit_app PRIVATE include)

if(BENCH_BACKEND STREQUAL "MICROTEL")
    # When built as a subdirectory of the microtel repo (MICROTEL_BUILD_BENCH=ON),
    # microtel_sdk is already defined in the parent scope — no find_package needed.
    # When built standalone (SUT Dockerfile stage 3), CMAKE_PREFIX_PATH points to
    # the installed microtel tree (issue #19 tracks adding install() targets).
    if(NOT TARGET microtel_sdk)
        find_package(microtel REQUIRED)
    endif()
    target_sources(emit_app PRIVATE src/backend_microtel.cpp)
    target_link_libraries(emit_app PRIVATE microtel_sdk)
    target_include_directories(emit_app PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src)

elseif(BENCH_BACKEND STREQUAL "OTELCPP_GRPC")
    find_package(opentelemetry-cpp REQUIRED COMPONENTS trace otlp_grpc_exporter)
    target_sources(emit_app PRIVATE src/backend_otelcpp.cpp)
    target_link_libraries(emit_app PRIVATE
        opentelemetry-cpp::trace
        opentelemetry-cpp::otlp_grpc_exporter)

elseif(BENCH_BACKEND STREQUAL "OTELCPP_HTTP")
    find_package(CURL REQUIRED)
    find_package(opentelemetry-cpp REQUIRED COMPONENTS trace otlp_http_exporter)
    target_sources(emit_app PRIVATE src/backend_otelcpp.cpp)
    target_link_libraries(emit_app PRIVATE
        opentelemetry-cpp::trace
        opentelemetry-cpp::otlp_http_exporter)

else()
    message(FATAL_ERROR "Unknown BENCH_BACKEND: ${BENCH_BACKEND}. "
        "Valid values: MICROTEL | OTELCPP_GRPC | OTELCPP_HTTP")
endif()
