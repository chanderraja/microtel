# Graph Report - /home/chander/repos/microtel  (2026-05-09)

## Corpus Check
- Large corpus: 260 files · ~168,924 words. Semantic extraction will be expensive (many Claude tokens). Consider running on a subfolder, or use --no-semantic to run AST-only.

## Summary
- 1623 nodes · 3488 edges · 157 communities (124 shown, 33 thin omitted)
- Extraction: 69% EXTRACTED · 31% INFERRED · 0% AMBIGUOUS · INFERRED: 1094 edges (avg confidence: 0.8)
- Token cost: 267,703 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_upb Fast Decoder|upb Fast Decoder]]
- [[_COMMUNITY_Project Architecture & Concepts|Project Architecture & Concepts]]
- [[_COMMUNITY_OTLP Encoder|OTLP Encoder]]
- [[_COMMUNITY_upb Status & Arena API|upb Status & Arena API]]
- [[_COMMUNITY_SDK Samplers|SDK Samplers]]
- [[_COMMUNITY_Common Proto Accessors|Common Proto Accessors]]
- [[_COMMUNITY_Trace Proto Accessors|Trace Proto Accessors]]
- [[_COMMUNITY_upb Message Field Accessors|upb Message Field Accessors]]
- [[_COMMUNITY_upb Message Allocation|upb Message Allocation]]
- [[_COMMUNITY_upb Array & Resource Proto|upb Array & Resource Proto]]
- [[_COMMUNITY_upb Memory Allocator|upb Memory Allocator]]
- [[_COMMUNITY_upb Internal Utilities|upb Internal Utilities]]
- [[_COMMUNITY_upb Hash Table Core|upb Hash Table Core]]
- [[_COMMUNITY_upb StringView & AnyValue|upb StringView & AnyValue]]
- [[_COMMUNITY_upb Unknown Fields|upb Unknown Fields]]
- [[_COMMUNITY_upb Array Operations|upb Array Operations]]
- [[_COMMUNITY_upb Message Clear & Compare|upb Message Clear & Compare]]
- [[_COMMUNITY_upb Mini Table Encoder|upb Mini Table Encoder]]
- [[_COMMUNITY_upb Field & Extension Lookup|upb Field & Extension Lookup]]
- [[_COMMUNITY_OTLP Collector Service|OTLP Collector Service]]
- [[_COMMUNITY_Design Decisions & ICPs|Design Decisions & ICPs]]
- [[_COMMUNITY_upb String Table|upb String Table]]
- [[_COMMUNITY_upb Map Sorting|upb Map Sorting]]
- [[_COMMUNITY_CI Header Check & Wire Codec|CI Header Check & Wire Codec]]
- [[_COMMUNITY_Test Fakes & Diagnostics|Test Fakes & Diagnostics]]
- [[_COMMUNITY_upb Extension & String Table|upb Extension & String Table]]
- [[_COMMUNITY_Proto Array Resize|Proto Array Resize]]
- [[_COMMUNITY_Epoll Reactor|Epoll Reactor]]
- [[_COMMUNITY_HTTP2 Transport Design|HTTP/2 Transport Design]]
- [[_COMMUNITY_Proto Mutable Array Access|Proto Mutable Array Access]]
- [[_COMMUNITY_SDK Builder & Log Sink|SDK Builder & Log Sink]]
- [[_COMMUNITY_upb String Table Iteration|upb String Table Iteration]]
- [[_COMMUNITY_upb MiniTable Field Metadata|upb MiniTable Field Metadata]]
- [[_COMMUNITY_Test Taxonomy & Interfaces|Test Taxonomy & Interfaces]]
- [[_COMMUNITY_upb MiniTable Message Metadata|upb MiniTable Message Metadata]]
- [[_COMMUNITY_Vendored Proto Files|Vendored Proto Files]]
- [[_COMMUNITY_gRPC Wire Protocol|gRPC Wire Protocol]]
- [[_COMMUNITY_upb Hash Values|upb Hash Values]]
- [[_COMMUNITY_SDK Interface Mocks|SDK Interface Mocks]]
- [[_COMMUNITY_upb Message Equality|upb Message Equality]]
- [[_COMMUNITY_UTF-8 Validation|UTF-8 Validation]]
- [[_COMMUNITY_Simple Span Processor|Simple Span Processor]]
- [[_COMMUNITY_Trace IDs & Flags|Trace IDs & Flags]]
- [[_COMMUNITY_HTTP Wire Codec Design|HTTP Wire Codec Design]]
- [[_COMMUNITY_upb Hash Table Internals|upb Hash Table Internals]]
- [[_COMMUNITY_upb OneOf & MiniTable Compat|upb OneOf & MiniTable Compat]]
- [[_COMMUNITY_Config & Auth Providers|Config & Auth Providers]]
- [[_COMMUNITY_Exporter Interface & Tests|Exporter Interface & Tests]]
- [[_COMMUNITY_OTLP Encoder Tests|OTLP Encoder Tests]]
- [[_COMMUNITY_API Design Decisions|API Design Decisions]]
- [[_COMMUNITY_Epoll Reactor Tests|Epoll Reactor Tests]]
- [[_COMMUNITY_Clock Interfaces|Clock Interfaces]]
- [[_COMMUNITY_Span Processor Interface|Span Processor Interface]]
- [[_COMMUNITY_Transport Interface|Transport Interface]]
- [[_COMMUNITY_Batch & Span Events|Batch & Span Events]]
- [[_COMMUNITY_Parent-Based Sampler Tests|Parent-Based Sampler Tests]]
- [[_COMMUNITY_Public Sampler API|Public Sampler API]]
- [[_COMMUNITY_Internal Sampler Interface|Internal Sampler Interface]]
- [[_COMMUNITY_upb Decode Depth Limit|upb Decode Depth Limit]]
- [[_COMMUNITY_upb Encode Depth Limit|upb Encode Depth Limit]]
- [[_COMMUNITY_Trace Ratio Sampler Tests|Trace Ratio Sampler Tests]]
- [[_COMMUNITY_Mock Span Processor|Mock Span Processor]]
- [[_COMMUNITY_Fake Span Processor|Fake Span Processor]]
- [[_COMMUNITY_W3C Propagator|W3C Propagator]]
- [[_COMMUNITY_Provider Public API|Provider Public API]]
- [[_COMMUNITY_Error Types|Error Types]]
- [[_COMMUNITY_Public Span API|Public Span API]]
- [[_COMMUNITY_Tracer Public API|Tracer Public API]]
- [[_COMMUNITY_Resource Public API|Resource Public API]]
- [[_COMMUNITY_Diagnostics Sink Interface|Diagnostics Sink Interface]]
- [[_COMMUNITY_Exporter Interface|Exporter Interface]]
- [[_COMMUNITY_Auth Provider Interface|Auth Provider Interface]]
- [[_COMMUNITY_Resource Detector Interface|Resource Detector Interface]]
- [[_COMMUNITY_Encoded Payload Type|Encoded Payload Type]]
- [[_COMMUNITY_OtlpEncoder Class|OtlpEncoder Class]]
- [[_COMMUNITY_UniqueFd RAII|UniqueFd RAII]]
- [[_COMMUNITY_Mock Exporter|Mock Exporter]]
- [[_COMMUNITY_Mock Sampler|Mock Sampler]]
- [[_COMMUNITY_Mock Transport|Mock Transport]]
- [[_COMMUNITY_Fake Clock|Fake Clock]]
- [[_COMMUNITY_Fake Steady Clock|Fake Steady Clock]]
- [[_COMMUNITY_Fake Diagnostics Sink|Fake Diagnostics Sink]]
- [[_COMMUNITY_Fake Auth Provider|Fake Auth Provider]]
- [[_COMMUNITY_Fake Resource Detector|Fake Resource Detector]]
- [[_COMMUNITY_Fake Exporter|Fake Exporter]]
- [[_COMMUNITY_Fake Reactor|Fake Reactor]]
- [[_COMMUNITY_Fake Transport|Fake Transport]]
- [[_COMMUNITY_Resource Attributes Concept|Resource Attributes Concept]]
- [[_COMMUNITY_Proto Directory Readme|Proto Directory Readme]]
- [[_COMMUNITY_Generated Code Readme|Generated Code Readme]]
- [[_COMMUNITY_Response Fuzz Target|Response Fuzz Target]]
- [[_COMMUNITY_OTLP Response Fuzz|OTLP Response Fuzz]]

## God Nodes (most connected - your core abstractions)
1. `upb_Message_ClearBaseField()` - 69 edges
2. `upb_Message_SetBaseField()` - 63 edges
3. `_upb_Message_GetNonExtensionField()` - 57 edges
4. `UPB_PRIVATE()` - 53 edges
5. `_upb_Message_New()` - 45 edges
6. `upb_Decode()` - 44 edges
7. `upb_MiniTableField_CType()` - 38 edges
8. `upb_Message_GetArray()` - 37 edges
9. `upb_Message_GetOrCreateMutableArray()` - 36 edges
10. `TEST_F()` - 35 edges

## Surprising Connections (you probably didn't know these)
- `opentelemetry_proto_common_v1_AnyValue_value_case()` --calls--> `upb_Message_WhichOneofFieldNumber()`  [INFERRED]
  gen/opentelemetry/proto/common/v1/common.upb.h → third_party/upb/upb/message/internal/accessors.h
- `opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_rejected_spans()` --calls--> `_upb_Message_GetNonExtensionField()`  [INFERRED]
  gen/opentelemetry/proto/collector/trace/v1/trace_service.upb.h → third_party/upb/upb/message/internal/accessors.h
- `opentelemetry_proto_common_v1_AnyValue_string_value_strindex()` --calls--> `_upb_Message_GetNonExtensionField()`  [INFERRED]
  gen/opentelemetry/proto/common/v1/common.upb.h → third_party/upb/upb/message/internal/accessors.h
- `opentelemetry_proto_common_v1_KeyValue_key_strindex()` --calls--> `_upb_Message_GetNonExtensionField()`  [INFERRED]
  gen/opentelemetry/proto/common/v1/common.upb.h → third_party/upb/upb/message/internal/accessors.h
- `opentelemetry_proto_common_v1_InstrumentationScope_dropped_attributes_count()` --calls--> `_upb_Message_GetNonExtensionField()`  [INFERRED]
  gen/opentelemetry/proto/common/v1/common.upb.h → third_party/upb/upb/message/internal/accessors.h

## Hyperedges (group relationships)
- **M0 Deliverable Documents** — docs_architecture, docs_threading_model, docs_memory_model, docs_error_model, docs_interfaces, docs_coding_standards, docs_grpc_wire_protocol, docs_configuration, docs_development, docs_repository_layout, docs_ci_architecture [EXTRACTED 1.00]
- **Twelve Locked Internal Interfaces** — concept_itransport, concept_iotlp_encoder, concept_iwire_codec, concept_iexporter, concept_isampler, concept_ispan_processor, concept_iclock, concept_ireactor, concept_iauth_provider, concept_iresource_detector, concept_idiagnostics_sink, concept_ilog_sink [EXTRACTED 1.00]
- **Six Parallel Implementation Tracks** — concept_trace_sdk, concept_otlp_http, concept_otlp_grpc, concept_nghttp2_transport, concept_upb_encoder, docs_configuration [EXTRACTED 1.00]
- **v1 Runtime Dependency Closure** — concept_nghttp2_transport, concept_openssl_tls, concept_upb_encoder, concept_spdlog_logging [EXTRACTED 1.00]
- **Custom RAII Wrapper Types** — concept_raii_wrappers, concept_itransport, concept_iotlp_encoder [EXTRACTED 1.00]
- **M0 normative sequence diagrams (8 total per ICP 0001)** — seq_connection_establishment, seq_retry_after_failure, seq_goaway_handling, seq_shutdown_drain, seq_fork_survival, seq_backpressure_and_drop, seq_partial_success, seq_grpc_trailer_multi_frame [EXTRACTED 1.00]
- **Vendored third-party libraries (tl-expected, upb, utf8_range)** — tl_expected_library, upb_library, utf8_range_library [EXTRACTED 1.00]
- **Track A source directories (api, sdk, exporter)** — src_api_readme, src_sdk_readme, src_exporter_readme [EXTRACTED 1.00]
- **M0 ICPs (0001, 0002, 0003)** — icp_0001, icp_0002, icp_0003 [EXTRACTED 1.00]
- **OTLP/HTTP Codec Dependencies** — otlp_http_codec, encoded_payload, itransport_interface, iauth_provider_interface, idiagnostics_sink_interface [EXTRACTED 1.00]
- **OTLP/gRPC Codec Dependencies** — otlp_grpc_codec, encoded_payload, itransport_interface, iauth_provider_interface, idiagnostics_sink_interface, google_rpc_status_proto, upb_dep [EXTRACTED 1.00]
- **RAII Wrappers Group** — raii_wrappers, socket_raii, sslctx_raii, sslsession_raii, nghttp2session_raii [EXTRACTED 1.00]
- **Vendored OTLP Proto Files** — proto_vendored, proto_common, proto_resource, proto_trace, proto_trace_service [EXTRACTED 1.00]
- **Required Fuzz Harnesses** — grpc_codec_fuzz, toml_fuzz, response_decompression_fuzz, otlp_response_fuzz [EXTRACTED 1.00]

## Communities (157 total, 33 thin omitted)

### Community 0 - "upb Fast Decoder"
Cohesion: 0.05
Nodes (90): fastdecode_checktag(), fastdecode_commitarr(), fastdecode_delimited(), fastdecode_dispatch(), fastdecode_fieldmem(), fastdecode_flippacked(), fastdecode_getfield(), fastdecode_isdonefallback() (+82 more)

### Community 1 - "Project Architecture & Concepts"
Cohesion: 0.05
Nodes (87): CLAUDE.md Agent Instructions, Apache 2.0 License, BatchSpanProcessor, microtel-bench Benchmark Harness, Compatibility Shims (otelcpp, otel-python), OpenTelemetry Compatibility Tiers (1-4), Config Precedence (code > env > toml > defaults), Control Plane (microtelctl, hot reload, v1.1) (+79 more)

### Community 2 - "OTLP Encoder"
Cohesion: 0.05
Nodes (69): EncodeAnyValueBool(), EncodeAnyValueDouble(), EncodeAnyValueInt(), EncodeAnyValueStr(), EncodeArrayBool(), EncodeArrayDouble(), EncodeArrayInt(), EncodeArrayStr() (+61 more)

### Community 3 - "upb Status & Arena API"
Cohesion: 0.07
Nodes (53): upb_Status_SetErrorFormat(), upb_Status_SetErrorMessage(), upb_Status_VAppendErrorFormat(), upb_Status_VSetErrorFormat(), upb_Arena_Malloc(), upb_Arena_Realloc(), upb_Arena_ShrinkLast(), UPB_PRIVATE() (+45 more)

### Community 4 - "SDK Samplers"
Cohesion: 0.05
Nodes (36): AlwaysOffSampler, AlwaysOnSampler, Get(), ParentBasedSampler, TraceIdRatioSampler, and_then(), and_then_impl(), assign() (+28 more)

### Community 5 - "Common Proto Accessors"
Cohesion: 0.04
Nodes (44): upb_Message_HasBaseField(), opentelemetry_proto_common_v1_AnyValue_clear_array_value(), opentelemetry_proto_common_v1_AnyValue_clear_bool_value(), opentelemetry_proto_common_v1_AnyValue_clear_bytes_value(), opentelemetry_proto_common_v1_AnyValue_clear_double_value(), opentelemetry_proto_common_v1_AnyValue_clear_int_value(), opentelemetry_proto_common_v1_AnyValue_clear_kvlist_value(), opentelemetry_proto_common_v1_AnyValue_clear_string_value() (+36 more)

### Community 6 - "Trace Proto Accessors"
Cohesion: 0.06
Nodes (42): upb_Message_ClearBaseField(), opentelemetry_proto_trace_v1_ResourceSpans_clear_resource(), opentelemetry_proto_trace_v1_ResourceSpans_clear_schema_url(), opentelemetry_proto_trace_v1_ResourceSpans_clear_scope_spans(), opentelemetry_proto_trace_v1_ResourceSpans_mutable_resource(), opentelemetry_proto_trace_v1_ResourceSpans_set_resource(), opentelemetry_proto_trace_v1_ScopeSpans_clear_schema_url(), opentelemetry_proto_trace_v1_ScopeSpans_clear_scope() (+34 more)

### Community 7 - "upb Message Field Accessors"
Cohesion: 0.13
Nodes (51): upb_Message_GetBool(), upb_Message_GetDouble(), _upb_Message_GetExtensionField(), upb_Message_GetField(), upb_Message_GetFloat(), upb_Message_GetInt32(), upb_Message_GetInt64(), _upb_Message_GetOrCreateMutableMap() (+43 more)

### Community 8 - "upb Message Allocation"
Cohesion: 0.06
Nodes (49): _upb_Message_New(), opentelemetry_proto_common_v1_AnyValue_new(), opentelemetry_proto_common_v1_AnyValue_parse(), opentelemetry_proto_common_v1_AnyValue_parse_ex(), opentelemetry_proto_common_v1_ArrayValue_new(), opentelemetry_proto_common_v1_ArrayValue_parse(), opentelemetry_proto_common_v1_ArrayValue_parse_ex(), opentelemetry_proto_common_v1_EntityRef_new() (+41 more)

### Community 9 - "upb Array & Resource Proto"
Cohesion: 0.06
Nodes (45): upb_Message_GetArray(), upb_Array_DataPtr(), opentelemetry_proto_common_v1_ArrayValue_values(), _opentelemetry_proto_common_v1_ArrayValue_values_upb_array(), opentelemetry_proto_common_v1_EntityRef_description_keys(), _opentelemetry_proto_common_v1_EntityRef_description_keys_upb_array(), opentelemetry_proto_common_v1_EntityRef_id_keys(), _opentelemetry_proto_common_v1_EntityRef_id_keys_upb_array() (+37 more)

### Community 10 - "upb Memory Allocator"
Cohesion: 0.1
Nodes (39): _upb_mapsorter_destroy(), upb_free(), upb_gfree(), upb_gmalloc(), upb_grealloc(), upb_malloc(), upb_realloc(), _upb_Arena_AddBlock() (+31 more)

### Community 11 - "upb Internal Utilities"
Cohesion: 0.11
Nodes (39): upb_BigEndian32(), upb_BigEndian64(), upb_IsLittleEndian(), upb_MiniTableField_IsPacked(), _upb_mapsorter_init(), _upb_mapsorter_popmap(), _upb_sortedmap_nextext(), UPB_PRIVATE() (+31 more)

### Community 12 - "upb Hash Table Core"
Cohesion: 0.12
Nodes (39): begin(), check(), init(), inthash(), intkey(), inttable_val(), inttable_val_const(), is_pow2() (+31 more)

### Community 13 - "upb StringView & AnyValue"
Cohesion: 0.1
Nodes (42): upb_StringView_FromString(), _upb_Message_GetNonExtensionField(), opentelemetry_proto_common_v1_AnyValue_bool_value(), opentelemetry_proto_common_v1_AnyValue_bytes_value(), opentelemetry_proto_common_v1_AnyValue_double_value(), opentelemetry_proto_common_v1_AnyValue_int_value(), opentelemetry_proto_common_v1_AnyValue_string_value(), opentelemetry_proto_common_v1_EntityRef_schema_url() (+34 more)

### Community 14 - "upb Unknown Fields"
Cohesion: 0.09
Nodes (33): UPB_PRIVATE(), upb_UnknownField_Compare(), upb_UnknownField_DoCompare(), upb_UnknownFields_Build(), upb_UnknownFields_DoBuild(), upb_UnknownFields_Grow(), upb_UnknownFields_IsEqual(), upb_UnknownFields_Merge() (+25 more)

### Community 15 - "upb Array Operations"
Cohesion: 0.14
Nodes (33): upb_Message_GetMutableArray(), upb_Array_IsFrozen(), upb_Array_MutableDataPtr(), upb_Array_Reserve(), upb_Array_Size(), UPB_PRIVATE(), upb_Array_Append(), upb_Array_Delete() (+25 more)

### Community 16 - "upb Message Clear & Compare"
Cohesion: 0.12
Nodes (28): upb_Message_Clear(), upb_Message_ClearExtension(), upb_Message_GetMessage(), upb_Message_GetMutableMessage(), UPB_PRIVATE(), upb_Message_IsFrozen(), UPB_PRIVATE(), upb_Message_IsEmpty() (+20 more)

### Community 17 - "upb Mini Table Encoder"
Cohesion: 0.15
Nodes (27): upb_FieldType_CType(), upb_FieldType_IsPackable(), _upb_Base92_DecodeVarint(), _upb_FromBase92(), _upb_ToBase92(), upb_MtDataEncoder_EncodeExtension(), upb_MtDataEncoder_EncodeMap(), upb_MtDataEncoder_EncodeMessageSet() (+19 more)

### Community 18 - "upb Field & Extension Lookup"
Cohesion: 0.16
Nodes (25): upb_StringView_FromDataAndSize(), upb_Message_GetMap(), upb_Message_GetMutableMap(), upb_MiniTableExtension_GetSubMessage(), upb_MiniTableField_Number(), upb_MiniTable_FieldCount(), upb_MiniTable_GetFieldByIndex(), upb_MiniTable_MapKey() (+17 more)

### Community 19 - "OTLP Collector Service"
Cohesion: 0.09
Nodes (22): Encode(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_clear_error_message(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_clear_rejected_spans(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_new(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_parse(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_parse_ex(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_rejected_spans(), opentelemetry_proto_collector_trace_v1_ExportTracePartialSuccess_set_error_message() (+14 more)

### Community 20 - "Design Decisions & ICPs"
Cohesion: 0.09
Nodes (28): Backpressure drop policies: drop_newest and drop_oldest, Fork survival: atfork handler closes all Providers in child, gRPC response parser state machine, microtel Starting Prompt (M0 Kickoff), ICP 0001: M0 Deliverables Clarification, ICP 0002: Vendor tl::expected as microtel::Expected, M0 deliverables scope (8 sequences, public headers, config+dev docs), microtel::Expected<T,E> wrapper type (+20 more)

### Community 21 - "upb String Table"
Cohesion: 0.14
Nodes (22): upb_strtable_clear(), upb_strtable_remove2(), upb_strtable_setentryvalue(), _upb_Map_Clear(), _upb_Map_CTypeSize(), _upb_Map_Delete(), _upb_map_fromvalue(), _upb_Map_Get() (+14 more)

### Community 22 - "upb Map Sorting"
Cohesion: 0.18
Nodes (18): upb_tabstrview(), _upb_sortedmap_next(), _upb_map_fromkey(), _upb_Map_Size(), _upb_mapsorter_cmpbool(), _upb_mapsorter_cmpext(), _upb_mapsorter_cmpi32(), _upb_mapsorter_cmpi64() (+10 more)

### Community 23 - "CI Header Check & Wire Codec"
Cohesion: 0.14
Nodes (8): microtel::ConfigError, EncodedPayload, IOtlpEncoder, IWireCodec, MockOtlpEncoder, MockWireCodec, SdkBuilder, WireResult

### Community 24 - "Test Fakes & Diagnostics"
Cohesion: 0.12
Nodes (16): DiagnosticsSink Implementation, FakeAuthProvider, FakeClock, FakeDiagnosticsSink, FakeReactor, FakeResourceDetector, FakeSteadyClock, HealthSnapshot (+8 more)

### Community 25 - "upb Extension & String Table"
Cohesion: 0.18
Nodes (14): isfull(), strcopy(), strkey2(), upb_strtable_init(), upb_strtable_insert(), upb_strtable_lookup2(), upb_MiniTableExtension_Number(), upb_Message_FindExtensionByNumber() (+6 more)

### Community 26 - "Proto Array Resize"
Cohesion: 0.12
Nodes (17): upb_Message_ResizeArrayUninitialized(), opentelemetry_proto_common_v1_ArrayValue_resize_values(), opentelemetry_proto_common_v1_EntityRef_resize_description_keys(), opentelemetry_proto_common_v1_EntityRef_resize_id_keys(), opentelemetry_proto_common_v1_InstrumentationScope_resize_attributes(), opentelemetry_proto_common_v1_KeyValueList_resize_values(), opentelemetry_proto_resource_v1_Resource_resize_attributes(), opentelemetry_proto_resource_v1_Resource_resize_entity_refs() (+9 more)

### Community 27 - "Epoll Reactor"
Cohesion: 0.15
Nodes (10): HasEvent(), IReactor, Create(), EpollReactor(), FromEpollEvents(), Modify(), OsError(), Register() (+2 more)

### Community 28 - "HTTP/2 Transport Design"
Cohesion: 0.14
Nodes (17): Connection State Machine, docs/error-model.md, HTTP/2 Transport (nghttp2+OpenSSL), docs/memory-model.md, nghttp2 dependency, Nghttp2Session RAII Wrapper, OpenSSL dependency, RAII Wrappers (Socket, SslCtx, SslSession, Nghttp2Session) (+9 more)

### Community 29 - "Proto Mutable Array Access"
Cohesion: 0.12
Nodes (16): upb_Message_GetOrCreateMutableArray(), _opentelemetry_proto_common_v1_ArrayValue_values_mutable_upb_array(), opentelemetry_proto_common_v1_EntityRef_add_description_keys(), opentelemetry_proto_common_v1_EntityRef_add_id_keys(), _opentelemetry_proto_common_v1_EntityRef_description_keys_mutable_upb_array(), _opentelemetry_proto_common_v1_EntityRef_id_keys_mutable_upb_array(), _opentelemetry_proto_common_v1_InstrumentationScope_attributes_mutable_upb_array(), _opentelemetry_proto_common_v1_KeyValueList_values_mutable_upb_array() (+8 more)

### Community 30 - "SDK Builder & Log Sink"
Cohesion: 0.15
Nodes (7): LevelTag(), LogImpl(), LogSink Injection Hook, SdkBuilder, LogSinkTest, SpawnLoggers(), TEST_F()

### Community 31 - "upb String Table Iteration"
Cohesion: 0.18
Nodes (12): upb_strtable_done(), upb_strtable_iter_isequal(), upb_strtable_iter_key(), upb_strtable_iter_value(), upb_strtable_next(), str_tabent(), upb_strtable_lookup(), upb_strtable_remove() (+4 more)

### Community 32 - "upb MiniTable Field Metadata"
Cohesion: 0.23
Nodes (10): upb_Message_HasExtension(), upb_MiniTableField_HasPresence(), upb_MiniTableField_IsArray(), upb_MiniTableField_IsClosedEnum(), upb_MiniTableField_IsInOneof(), upb_MiniTableField_IsMap(), upb_MiniTableField_IsSubMessage(), upb_MiniTableField_Type() (+2 more)

### Community 33 - "Test Taxonomy & Interfaces"
Cohesion: 0.15
Nodes (14): Conformance Tier 1 (OTLP wire-protocol compliance), Dumb Mock Contract, Fake Test Double Contract, docs/interfaces.md, Interop Matrix, OpenTelemetry Collector (otel/opentelemetry-collector:0.151.0), Test Taxonomy (unit/integration/conformance/wire/grpc-wire/fuzz), tests/conformance/ README (+6 more)

### Community 34 - "upb MiniTable Message Metadata"
Cohesion: 0.27
Nodes (10): upb_Message_SetClosedEnum(), upb_Message_LogNewMessage(), upb_MiniTable_FieldIsLinked(), upb_MiniTable_FullName(), upb_MiniTable_GetSubEnumTable(), upb_MiniTable_GetSubMessageTable(), upb_MiniTable_MapEntrySubMessage(), upb_MiniTable_SetFullName() (+2 more)

### Community 35 - "Vendored Proto Files"
Cohesion: 0.18
Nodes (12): ExportTraceServiceRequest, Generated upb C Accessors, opentelemetry-proto upstream (GitHub), opentelemetry/proto/common/v1/common.proto, opentelemetry/proto/resource/v1/resource.proto, opentelemetry/proto/trace/v1/trace.proto, opentelemetry/proto/collector/trace/v1/trace_service.proto, Vendored opentelemetry-proto (v1.10.0) (+4 more)

### Community 36 - "gRPC Wire Protocol"
Cohesion: 0.18
Nodes (12): google.rpc.Status proto, gRPC 5-byte Length-Prefix Framing, grpc_codec_fuzz.cpp, gRPC Codec State Machine, gRPC Status Code Matrix (error-model §7.2), docs/grpc-wire-protocol.md, OTLP/gRPC Wire Codec, google.rpc.RetryInfo Decoding (+4 more)

### Community 37 - "upb Hash Values"
Cohesion: 0.22
Nodes (8): streql(), upb_tabstr(), upb_value_double(), upb_value_float(), upb_value_setdouble(), upb_value_setfloat(), _upb_msg_map_key(), _upb_msg_map_value()

### Community 38 - "SDK Interface Mocks"
Cohesion: 0.18
Nodes (9): FakeSpanProcessor, IOtlpEncoder Interface, ISampler Interface, ISpanProcessor Interface, IWireCodec Interface, MockOtlpEncoder, MockSampler, MockSpanProcessor (+1 more)

### Community 39 - "upb Message Equality"
Cohesion: 0.4
Nodes (7): upb_StringView_IsEqual(), _upb_Array_IsEqual(), _upb_Map_IsEqual(), _upb_Message_BaseFieldsAreEqual(), _upb_Message_ExtensionsAreEqual(), upb_Message_IsEqual(), upb_MessageValue_IsEqual()

### Community 40 - "UTF-8 Validation"
Cohesion: 0.42
Nodes (9): utf8_range_AsciiIsAscii(), utf8_range_CodepointSkipBackwards(), utf8_range_IsTrailByteOk(), utf8_range_IsValid(), utf8_range_SkipAscii(), utf8_range_UnalignedLoad64(), utf8_range_Validate(), utf8_range_ValidateUTF8Naive() (+1 more)

### Community 41 - "Simple Span Processor"
Cohesion: 0.2
Nodes (3): Context, SimpleSpanProcessor, Span

### Community 42 - "Trace IDs & Flags"
Cohesion: 0.22
Nodes (4): SpanId, TraceFlags, TraceId, TraceState

### Community 43 - "HTTP Wire Codec Design"
Cohesion: 0.31
Nodes (9): docs/configuration.md, FakeTransport, HTTP Status Code Matrix (error-model §7.1), ITransport Interface, MockTransport, OTLP/HTTP Wire Codec, Retry-After Header Parsing, src/wire/http/ README (+1 more)

### Community 44 - "upb Hash Table Internals"
Cohesion: 0.36
Nodes (8): emptyent(), findentry(), findentry_mutable(), getentry_mutable(), insert(), lookup(), upb_getentry(), upb_tabent_isempty()

### Community 45 - "upb OneOf & MiniTable Compat"
Cohesion: 0.36
Nodes (7): upb_Message_ClearOneof(), upb_Message_WhichOneof(), upb_Message_WhichOneofFieldNumber(), upb_deep_check(), upb_MiniTable_Compatible(), upb_MiniTable_Equals(), upb_MiniTable_FindFieldByNumber()

### Community 46 - "Config & Auth Providers"
Cohesion: 0.32
Nodes (8): CallbackAuthProvider, Config Implementation, IAuthProvider Interface, src/common/config/ README, StaticHeadersAuthProvider, toml_fuzz.cpp, TOML Parser Library (toml++ or cpptoml), Track E — Configuration

### Community 48 - "Exporter Interface & Tests"
Cohesion: 0.33
Nodes (6): FakeExporter, IExporter Interface, MockExporter, MakeSpanRecord(), SimpleSpanProcessorTest, TEST_F()

### Community 49 - "OTLP Encoder Tests"
Cohesion: 0.38
Nodes (6): MakeCtx(), MakeSpanId(), MakeTraceId(), NsEpoch(), OtlpEncoderTest, SvStr()

### Community 50 - "API Design Decisions"
Cohesion: 0.6
Nodes (6): ICP 0003: M0 Deferred Decisions, MPSC span-queue: bounded ring buffer (M3 default), SpanHandle: unique_ptr<Span, SpanDeleter>, src/api/ README, src/sdk/ README, SslCtx ownership: per-Transport (ICP 0003 §3.1)

### Community 51 - "Epoll Reactor Tests"
Cohesion: 0.53
Nodes (5): Deadline(), EpollReactorTest, MakePipe(), MustRegister(), TEST_F()

### Community 53 - "Span Processor Interface"
Cohesion: 0.4
Nodes (3): Context, ISpanProcessor, Span

### Community 55 - "Batch & Span Events"
Cohesion: 0.4
Nodes (3): BatchHandle, SpanEvent, SpanLink

### Community 56 - "Parent-Based Sampler Tests"
Cohesion: 0.7
Nodes (4): MakeCtxWithoutParent(), MakeCtxWithParent(), MakeParentContext(), TEST()

### Community 59 - "upb Decode Depth Limit"
Cohesion: 0.83
Nodes (3): upb_Decode_LimitDepth(), upb_DecodeOptions_GetMaxDepth(), upb_DecodeOptions_MaxDepth()

### Community 60 - "upb Encode Depth Limit"
Cohesion: 0.83
Nodes (3): upb_Encode_LimitDepth(), upb_EncodeOptions_GetMaxDepth(), upb_EncodeOptions_MaxDepth()

### Community 61 - "Trace Ratio Sampler Tests"
Cohesion: 0.83
Nodes (3): MakeCtx(), MakeTraceIdWithLowerBytes(), TEST()

### Community 62 - "Mock Span Processor"
Cohesion: 0.5
Nodes (3): Context, MockSpanProcessor, Span

### Community 63 - "Fake Span Processor"
Cohesion: 0.5
Nodes (3): Context, FakeSpanProcessor, Span

## Knowledge Gaps
- **112 isolated node(s):** `ISampler`, `Error`, `ConfigError`, `Span`, `Context` (+107 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **33 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `EncodedPayload` connect `CI Header Check & Wire Codec` to `Vendored Proto Files`, `OTLP Encoder`, `HTTP Wire Codec Design`, `gRPC Wire Protocol`?**
  _High betweenness centrality (0.156) - this node is a cross-community bridge._
- **Why does `upb_Arena_Malloc()` connect `upb Status & Arena API` to `upb MiniTable Message Metadata`, `upb Message Allocation`, `upb Memory Allocator`, `upb Hash Table Core`, `upb Unknown Fields`, `upb Array Operations`, `upb Field & Extension Lookup`, `upb String Table`, `upb Extension & String Table`?**
  _High betweenness centrality (0.081) - this node is a cross-community bridge._
- **Why does `_upb_Message_New()` connect `upb Message Allocation` to `upb Fast Decoder`, `upb MiniTable Message Metadata`, `upb Status & Arena API`, `OTLP Encoder`, `Common Proto Accessors`, `Trace Proto Accessors`, `upb Message Field Accessors`, `upb Array & Resource Proto`, `upb Message Clear & Compare`, `OTLP Collector Service`?**
  _High betweenness centrality (0.077) - this node is a cross-community bridge._
- **Are the 66 inferred relationships involving `upb_Message_ClearBaseField()` (e.g. with `upb_Message_IsFrozen()` and `upb_MiniTableField_IsInOneof()`) actually correct?**
  _`upb_Message_ClearBaseField()` has 66 INFERRED edges - model-reasoned connections that need verification._
- **Are the 51 inferred relationships involving `upb_Message_SetBaseField()` (e.g. with `upb_Message_Map_DeepClone()` and `upb_Message_Array_DeepClone()`) actually correct?**
  _`upb_Message_SetBaseField()` has 51 INFERRED edges - model-reasoned connections that need verification._
- **Are the 49 inferred relationships involving `_upb_Message_GetNonExtensionField()` (e.g. with `upb_MiniTableField_IsExtension()` and `upb_MiniTableField_IsInOneof()`) actually correct?**
  _`_upb_Message_GetNonExtensionField()` has 49 INFERRED edges - model-reasoned connections that need verification._
- **Are the 8 inferred relationships involving `UPB_PRIVATE()` (e.g. with `upb_MiniTableField_Number()` and `upb_BigEndian64()`) actually correct?**
  _`UPB_PRIVATE()` has 8 INFERRED edges - model-reasoned connections that need verification._