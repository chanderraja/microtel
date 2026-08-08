# microtel spdlog bridge — an opt-in adapter, not part of the core runtime.
#
# Header-only INTERFACE library: the SpdlogSink lives in
# include/microtel/adapters/spdlog_sink.hpp and depends only on the microtel
# public Logger interface plus spdlog. Applications link this in addition to
# microtel to route their spdlog logging into the OTLP logs pipeline.
#
# Only added to the build when MICROTEL_USE_SPDLOG is ON (see the root
# CMakeLists), so the core builds with zero spdlog dependency when it is OFF.

add_library(microtel_spdlog_bridge INTERFACE)

target_include_directories(microtel_spdlog_bridge INTERFACE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(microtel_spdlog_bridge INTERFACE spdlog::spdlog)

target_compile_features(microtel_spdlog_bridge INTERFACE cxx_std_20)
