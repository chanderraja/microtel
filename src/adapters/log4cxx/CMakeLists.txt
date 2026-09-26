# microtel log4cxx bridge (issue #304) — an opt-in adapter, not part of the core.
#
# Header-only INTERFACE library: the Log4cxxAppender lives in
# include/microtel/adapters/log4cxx_appender.hpp and depends only on the
# microtel public Logger interface plus log4cxx. Applications link this in
# addition to microtel to route their log4cxx output into the OTLP logs
# pipeline.
#
# Only added to the build when MICROTEL_BUILD_LOG4CXX_BRIDGE is ON (see the root
# CMakeLists). Not installed as a binary and not exported (ICP 0020 Decision 3):
# the header installs with the rest of include/microtel/, and a consumer
# compiles it against the log4cxx already in their build.

add_library(microtel_log4cxx_bridge INTERFACE)

target_include_directories(microtel_log4cxx_bridge INTERFACE ${CMAKE_SOURCE_DIR}/include)

# log4cxx's CMake package names its imported target plain `log4cxx`.
target_link_libraries(microtel_log4cxx_bridge INTERFACE log4cxx)

target_compile_features(microtel_log4cxx_bridge INTERFACE cxx_std_20)
