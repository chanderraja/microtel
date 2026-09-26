# microtel glog bridge (issue #304) — an opt-in adapter, not part of the core.
#
# Header-only INTERFACE library: the GlogSink lives in
# include/microtel/adapters/glog_sink.hpp and depends only on the microtel
# public Logger interface plus glog. Applications link this in addition to
# microtel to route their glog output into the OTLP logs pipeline.
#
# Only added to the build when MICROTEL_BUILD_GLOG_BRIDGE is ON (see the root
# CMakeLists). Not installed as a binary and not exported (ICP 0020 Decision 3):
# the header installs with the rest of include/microtel/, and a consumer
# compiles it against the glog already in their build.

add_library(microtel_glog_bridge INTERFACE)

target_include_directories(microtel_glog_bridge INTERFACE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(microtel_glog_bridge INTERFACE glog::glog)

target_compile_features(microtel_glog_bridge INTERFACE cxx_std_20)
