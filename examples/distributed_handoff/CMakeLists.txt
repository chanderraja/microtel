# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# distributed_handoff — W3C traceparent / tracestate / baggage across two
# processes. One directory, two binaries, which is what SOURCES is for. See
# README.md here, and examples/CMakeLists.txt for what
# microtel_add_example() does.
#
# Both link only microtel_sdk; wire.hpp is a header in this directory and needs
# no mention.

microtel_add_example(handoff_sender SOURCES sender.cpp)
microtel_add_example(handoff_receiver SOURCES receiver.cpp)
