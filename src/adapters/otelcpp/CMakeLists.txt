# microtel opentelemetry-cpp API-adapter shim (M17, ICP 0014).
#
# Opt-in via MICROTEL_BUILD_OTELCPP_SHIM; never part of the default build, so a
# consumer who does not ask for it never fetches opentelemetry-cpp headers.
#
# **Source-only, permanently.** This target exists so the shim can be built and
# tested in-tree. It is not an artifact microtel ships: the opentelemetry-cpp
# API encodes its nostd:: types and ABI version into every signature, so a
# prebuilt shim's ABI would depend on build choices made by the *consumer*.
# Consumers compile the shim inside their own build, where exactly one
# configuration is in play. See ICP 0014 §1.
#
# The shim is excluded from microtel's dependency-closure claims: it is not
# scanned as a shipped artifact by ci/scripts/symbol-scan.sh, and it does not
# appear in the M7 footprint numbers.

add_library(microtel_otelcpp_shim INTERFACE)

target_include_directories(microtel_otelcpp_shim INTERFACE ${CMAKE_SOURCE_DIR}/include)

# microtel_otelcpp_api carries the include path and the load-bearing
# OPENTELEMETRY_STL_VERSION=2020 definition (see the root CMakeLists).
target_link_libraries(microtel_otelcpp_shim INTERFACE microtel_otelcpp_api)

target_compile_features(microtel_otelcpp_shim INTERFACE cxx_std_20)
