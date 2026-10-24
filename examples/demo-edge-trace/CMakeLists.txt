# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# The gateway half of examples/demo-edge-trace (the firmware half is its own
# cross-compiled project in firmware/). Needs the concentrator.
if(MICROTEL_WITH_CONCENTRATOR)
    microtel_add_example(edge_gateway SOURCES gateway/edge_gateway.cpp)
else()
    message(STATUS "examples/demo-edge-trace: gateway skipped (needs -DMICROTEL_WITH_CONCENTRATOR=ON)")
endif()
