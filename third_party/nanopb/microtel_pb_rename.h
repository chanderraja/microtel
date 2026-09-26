// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Renames every globally visible symbol of the vendored nanopb runtime, and of
// the nanopb descriptors generated under gen/nanopb/, to a `microtel_pb_`
// prefix:
//
//   pb_encode                              -> microtel_pb_encode
//   opentelemetry_proto_trace_v1_Span_msg  -> microtel_pb_opentelemetry_proto_trace_v1_Span_msg
//
// WHY
// ---
// ICP 0031 Decision 4 and docs/leaf-concentrator-design.md §2.5. A firmware
// image that already links its own nanopb must not get two definitions of
// `pb_encode`: with static libraries the linker keeps whichever archive it
// reaches first and drops the other without a diagnostic, the hazard ICP 0020
// Decision 4 removed for upb.
//
// The generated descriptors (each message's `_msg`, `_field_info` and
// `_submsg_info`) are renamed too, unlike upb's generated names. A firmware
// that already generates OTLP with its own nanopb is exactly the leaf's
// audience, so that collision is likely rather than theoretical (design §9,
// decision 9).
//
// HOW IT IS APPLIED
// -----------------
// Force-included (`-include`) into every translation unit that reaches a
// nanopb header. third_party/nanopb/CMakeLists.txt attaches it to
// `microtel_nanopb` as a PUBLIC build-interface compile option, so
// `microtel_nanopb`, `microtel_nanopb_gen` and every target that links either
// one get it. A `#define` rewrites the identifier at every use, so
// declarations, definitions and call sites move together; a TU that misses the
// header references the upstream names and fails to link.
//
// The descriptor names are formed by token pasting in `PB_BIND` (pb.h). That
// does not defeat the rename: `##` suppresses expansion of its operands, but
// the pasted token is rescanned, and replaced here.
//
// The vendored sources and gen/nanopb/ stay byte-identical to upstream and to
// the generator's output.
//
// SCOPE: GLOBALS ONLY
// -------------------
// The list covers symbols with global linkage (`nm -g`), the set a linker can
// collide. File-local statics keep their upstream names. Undefined references
// to libc (`memcpy`, `memset`) are not nanopb's and are not renamed.
//
// REGENERATING AFTER A nanopb PIN BUMP OR A gen/nanopb/ CHANGE
// ------------------------------------------------------------
// `ci/scripts/symbol-scan.sh` fails on any unprefixed `pb_*` global and on any
// unprefixed generated descriptor, so the list is enforced rather than
// trusted. To rebuild it:
//
//   cmake -S . -B build -DMICROTEL_BUILD_TESTS=OFF -DMICROTEL_BUILD_LEAF=ON
//   cmake --build build --target microtel_nanopb microtel_nanopb_gen
//   find build -type f -name 'libmicrotel_nanopb*.a' -print0 |
//     xargs -0 nm -A -g 2>/dev/null |
//     sed 's/^[^:]*:[^:]*: *//; s/^[0-9a-fA-F]* //; s/^[A-Za-z] //' |
//     grep -E '^(microtel_)?(_?pb_|opentelemetry_proto_[A-Za-z0-9_]+_(msg|field_info|submsg_info)$)' |
//     sed 's/^microtel_//; s/^pb_opentelemetry_/opentelemetry_/' |
//     LC_ALL=C sort -u |
//     awk '/^pb_/ { printf "#define %s microtel_%s\n", $0, $0; next }
//          { printf "#define %s microtel_pb_%s\n", $0, $0 }'
//
// (Trailing `|` rather than `\` on purpose: a backslash at the end of a `//`
// line splices the next line into the comment.)
//
// and paste the output over the block below. The recipe reads a renamed build
// and strips the prefix before sorting, so it round-trips the current list and
// picks up anything new in the same pass. At this pin the global set is the
// same for clang and gcc, Debug and Release.

#ifndef MICROTEL_PB_RENAME_H_
#define MICROTEL_PB_RENAME_H_

// clang-format off
#define opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_field_info microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_field_info
#define opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_msg microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_msg
#define opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_submsg_info microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_submsg_info
#define opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_field_info microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_field_info
#define opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_msg microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_msg
#define opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_submsg_info microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_submsg_info
#define opentelemetry_proto_collector_trace_v1_ExportTraceServiceResponse_field_info microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTraceServiceResponse_field_info
#define opentelemetry_proto_collector_trace_v1_ExportTraceServiceResponse_msg microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTraceServiceResponse_msg
#define opentelemetry_proto_collector_trace_v1_ExportTraceServiceResponse_submsg_info microtel_pb_opentelemetry_proto_collector_trace_v1_ExportTraceServiceResponse_submsg_info
#define opentelemetry_proto_common_v1_AnyValue_field_info microtel_pb_opentelemetry_proto_common_v1_AnyValue_field_info
#define opentelemetry_proto_common_v1_AnyValue_msg microtel_pb_opentelemetry_proto_common_v1_AnyValue_msg
#define opentelemetry_proto_common_v1_AnyValue_submsg_info microtel_pb_opentelemetry_proto_common_v1_AnyValue_submsg_info
#define opentelemetry_proto_common_v1_ArrayValue_field_info microtel_pb_opentelemetry_proto_common_v1_ArrayValue_field_info
#define opentelemetry_proto_common_v1_ArrayValue_msg microtel_pb_opentelemetry_proto_common_v1_ArrayValue_msg
#define opentelemetry_proto_common_v1_ArrayValue_submsg_info microtel_pb_opentelemetry_proto_common_v1_ArrayValue_submsg_info
#define opentelemetry_proto_common_v1_EntityRef_field_info microtel_pb_opentelemetry_proto_common_v1_EntityRef_field_info
#define opentelemetry_proto_common_v1_EntityRef_msg microtel_pb_opentelemetry_proto_common_v1_EntityRef_msg
#define opentelemetry_proto_common_v1_EntityRef_submsg_info microtel_pb_opentelemetry_proto_common_v1_EntityRef_submsg_info
#define opentelemetry_proto_common_v1_InstrumentationScope_field_info microtel_pb_opentelemetry_proto_common_v1_InstrumentationScope_field_info
#define opentelemetry_proto_common_v1_InstrumentationScope_msg microtel_pb_opentelemetry_proto_common_v1_InstrumentationScope_msg
#define opentelemetry_proto_common_v1_InstrumentationScope_submsg_info microtel_pb_opentelemetry_proto_common_v1_InstrumentationScope_submsg_info
#define opentelemetry_proto_common_v1_KeyValueList_field_info microtel_pb_opentelemetry_proto_common_v1_KeyValueList_field_info
#define opentelemetry_proto_common_v1_KeyValueList_msg microtel_pb_opentelemetry_proto_common_v1_KeyValueList_msg
#define opentelemetry_proto_common_v1_KeyValueList_submsg_info microtel_pb_opentelemetry_proto_common_v1_KeyValueList_submsg_info
#define opentelemetry_proto_common_v1_KeyValue_field_info microtel_pb_opentelemetry_proto_common_v1_KeyValue_field_info
#define opentelemetry_proto_common_v1_KeyValue_msg microtel_pb_opentelemetry_proto_common_v1_KeyValue_msg
#define opentelemetry_proto_common_v1_KeyValue_submsg_info microtel_pb_opentelemetry_proto_common_v1_KeyValue_submsg_info
#define opentelemetry_proto_resource_v1_Resource_field_info microtel_pb_opentelemetry_proto_resource_v1_Resource_field_info
#define opentelemetry_proto_resource_v1_Resource_msg microtel_pb_opentelemetry_proto_resource_v1_Resource_msg
#define opentelemetry_proto_resource_v1_Resource_submsg_info microtel_pb_opentelemetry_proto_resource_v1_Resource_submsg_info
#define opentelemetry_proto_trace_v1_ResourceSpans_field_info microtel_pb_opentelemetry_proto_trace_v1_ResourceSpans_field_info
#define opentelemetry_proto_trace_v1_ResourceSpans_msg microtel_pb_opentelemetry_proto_trace_v1_ResourceSpans_msg
#define opentelemetry_proto_trace_v1_ResourceSpans_submsg_info microtel_pb_opentelemetry_proto_trace_v1_ResourceSpans_submsg_info
#define opentelemetry_proto_trace_v1_ScopeSpans_field_info microtel_pb_opentelemetry_proto_trace_v1_ScopeSpans_field_info
#define opentelemetry_proto_trace_v1_ScopeSpans_msg microtel_pb_opentelemetry_proto_trace_v1_ScopeSpans_msg
#define opentelemetry_proto_trace_v1_ScopeSpans_submsg_info microtel_pb_opentelemetry_proto_trace_v1_ScopeSpans_submsg_info
#define opentelemetry_proto_trace_v1_Span_Event_field_info microtel_pb_opentelemetry_proto_trace_v1_Span_Event_field_info
#define opentelemetry_proto_trace_v1_Span_Event_msg microtel_pb_opentelemetry_proto_trace_v1_Span_Event_msg
#define opentelemetry_proto_trace_v1_Span_Event_submsg_info microtel_pb_opentelemetry_proto_trace_v1_Span_Event_submsg_info
#define opentelemetry_proto_trace_v1_Span_Link_field_info microtel_pb_opentelemetry_proto_trace_v1_Span_Link_field_info
#define opentelemetry_proto_trace_v1_Span_Link_msg microtel_pb_opentelemetry_proto_trace_v1_Span_Link_msg
#define opentelemetry_proto_trace_v1_Span_Link_submsg_info microtel_pb_opentelemetry_proto_trace_v1_Span_Link_submsg_info
#define opentelemetry_proto_trace_v1_Span_field_info microtel_pb_opentelemetry_proto_trace_v1_Span_field_info
#define opentelemetry_proto_trace_v1_Span_msg microtel_pb_opentelemetry_proto_trace_v1_Span_msg
#define opentelemetry_proto_trace_v1_Span_submsg_info microtel_pb_opentelemetry_proto_trace_v1_Span_submsg_info
#define opentelemetry_proto_trace_v1_Status_field_info microtel_pb_opentelemetry_proto_trace_v1_Status_field_info
#define opentelemetry_proto_trace_v1_Status_msg microtel_pb_opentelemetry_proto_trace_v1_Status_msg
#define opentelemetry_proto_trace_v1_Status_submsg_info microtel_pb_opentelemetry_proto_trace_v1_Status_submsg_info
#define opentelemetry_proto_trace_v1_TracesData_field_info microtel_pb_opentelemetry_proto_trace_v1_TracesData_field_info
#define opentelemetry_proto_trace_v1_TracesData_msg microtel_pb_opentelemetry_proto_trace_v1_TracesData_msg
#define opentelemetry_proto_trace_v1_TracesData_submsg_info microtel_pb_opentelemetry_proto_trace_v1_TracesData_submsg_info
#define pb_default_field_callback microtel_pb_default_field_callback
#define pb_encode microtel_pb_encode
#define pb_encode_ex microtel_pb_encode_ex
#define pb_encode_fixed32 microtel_pb_encode_fixed32
#define pb_encode_fixed64 microtel_pb_encode_fixed64
#define pb_encode_string microtel_pb_encode_string
#define pb_encode_submessage microtel_pb_encode_submessage
#define pb_encode_svarint microtel_pb_encode_svarint
#define pb_encode_tag microtel_pb_encode_tag
#define pb_encode_tag_for_field microtel_pb_encode_tag_for_field
#define pb_encode_varint microtel_pb_encode_varint
#define pb_field_iter_begin microtel_pb_field_iter_begin
#define pb_field_iter_begin_const microtel_pb_field_iter_begin_const
#define pb_field_iter_begin_extension microtel_pb_field_iter_begin_extension
#define pb_field_iter_begin_extension_const microtel_pb_field_iter_begin_extension_const
#define pb_field_iter_find microtel_pb_field_iter_find
#define pb_field_iter_find_extension microtel_pb_field_iter_find_extension
#define pb_field_iter_next microtel_pb_field_iter_next
#define pb_get_encoded_size microtel_pb_get_encoded_size
#define pb_ostream_from_buffer microtel_pb_ostream_from_buffer
#define pb_write microtel_pb_write
// clang-format on

#endif // MICROTEL_PB_RENAME_H_
