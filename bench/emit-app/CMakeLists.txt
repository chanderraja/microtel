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

add_executable(emit_app src/main.cpp)

target_compile_definitions(emit_app PRIVATE
    BENCH_BACKEND_${BENCH_BACKEND}=1)

target_include_directories(emit_app PRIVATE include)

# Track 4 will add find_package() and target_link_libraries() calls here
# once the real workload and backend adapters are implemented.
