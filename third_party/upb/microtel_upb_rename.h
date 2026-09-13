// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Renames every globally-visible symbol of the vendored upb runtime and
// utf8_range to `microtel_<upstream name>`.
//
// WHY
// ---
// ICP 0020 Decision 4. microtel ships `libmicrotel_upb_runtime.a` and
// `libmicrotel_utf8_range.a` as part of its exported static-library set. A
// consumer who also links a real upb — directly, or transitively through
// protobuf or gRPC — would then have two definitions of `upb_Arena_Init`.
// With static libraries there is no diagnostic for that: the linker picks
// whichever archive it reaches first and silently discards the other, which
// is the worst category of defect to ship knowingly. Renaming makes the
// collision impossible by construction.
//
// Prefixing is cheap now and an ABI break later, which is why it is in scope
// for v1.0: after v1.0 it would break every consumer who linked the exported
// target.
//
// HOW IT IS APPLIED
// -----------------
// Force-included with `-include` as the first compile option on every target
// that compiles a translation unit reaching a upb header:
//
//   microtel_upb_runtime, microtel_utf8_range   third_party/
//   microtel_upb_gen                            gen/
//   microtel_encoder                            src/wire/encoder/
//   upb_arena_test, otlp_encoder_test,          tests/unit/wire/
//     otlp_metric_encoder_test, otlp_log_encoder_test
//   otelcpp_wire_conformance_test               tests/integration/otelcpp_shim/
//
// A `#define` rewrites the identifier at every use, so declarations,
// definitions and call sites all move together. Every upb-touching TU must
// see this header — a TU that misses it emits references to the upstream
// names and fails to link. No consumer of microtel ever sees it: the upb
// include paths and link dependencies are PRIVATE to the targets above, so no
// upb header reaches a public interface and there is no ODR hazard.
//
// SCOPE: GLOBALS ONLY
// -------------------
// The list covers symbols with global linkage (`nm -g`: defined, weak, and
// undefined), because that is exactly the set a linker can collide. File-local
// `static` helpers inside upb's .c files keep their upstream names; the linker
// never considers them when resolving a consumer's reference, and their
// presence depends on the compiler's inlining decisions, so including them
// would make the gate fire on `-O` level rather than on a real hazard.
//
// REGENERATING AFTER A upb PIN BUMP
// ---------------------------------
// A pin bump can add, remove, or rename globals. `ci/scripts/symbol-scan.sh`
// fails the build if any escape this list, so the list is enforced rather than
// trusted. To rebuild it:
//
//   cmake -S . -B build -DMICROTEL_BUILD_TESTS=OFF
//   cmake --build build
//   find build -type f -name 'libmicrotel_*.a' -print0 |
//     xargs -0 nm -A -C -g 2>/dev/null |
//     sed 's/^[^:]*:[^:]*: *//; s/^[0-9a-fA-F]* //; s/^[A-Za-z] //' |
//     grep -E '^(microtel_)?(_?upb_|_?kUpb_|kWyhashSalt$|UPB_linkarr|utf8_range_)' |
//     sed 's/^microtel_//; s/::.*$//' |
//     LC_ALL=C sort -u |
//     awk '{ printf "#define %s microtel_%s\n", $0, $0 }'
//
// (The pipeline is written with trailing `|` rather than `\` on purpose: a
// backslash at the end of a `//` line splices the next line into the comment,
// which gcc rejects under -Werror=comment.)
//
// and paste the output over the block below. The recipe reads a *renamed*
// build: it strips the `microtel_` prefix before sorting, so it round-trips
// the current list and picks up anything new in the same pass — there is no
// need to disable the rename first. Build without `-DCMAKE_BUILD_TYPE` (or in
// Debug): at `-O2` the compiler inlines away the `UPB_INLINE` functions, and
// the list comes out short. The two `sed` normalizations handle nm's rendering
// of a vague-linkage function-local static (`f()::x` — the enclosing function
// is what gets renamed).
//
// KNOWN RESIDUAL: THE `linkarr_upb_AllExts` SECTION
// -------------------------------------------------
// upb's linked-extension registry lives in an ELF section whose name is built
// by token-pasting and stringification in `upb/port/def.inc`. Reproduced here
// with the line continuations dropped (a `\` at the end of a `//` line splices
// the next line into the comment, which gcc rejects under -Werror=comment):
//
//   #define UPB_LINKARR_APPEND(name)
//     __attribute__((retain, used, section("linkarr_" #name)))
//   #define UPB_LINKARR_DECLARE(name, type)
//     extern type const __start_linkarr_##name;
//     extern type const __stop_linkarr_##name;
//     UPB_LINKARR_APPEND(name) type UPB_linkarr_internal_empty_##name[1]
//
// `#` and `##` suppress macro expansion of their operand, so a `#define` for
// `upb_AllExts` cannot reach the composed section name `linkarr_upb_AllExts`
// or the linker-synthesized `__start_/__stop_linkarr_upb_AllExts` bounds. The
// array symbol itself is composed the same way and so carries its own explicit
// entry below.
//
// Consequence: a consumer who links microtel *and* a real upb gets one shared
// `linkarr_upb_AllExts` section holding both extension registries, and
// `upb_ExtensionRegistry_AddAllLinkedExtensions` on either side would walk the
// concatenation. Accepted: microtel registers no extensions (its only entry is
// upb's zero-filled placeholder, which that walk already skips), the OTLP
// encoder never calls the linked-extension API, and renaming the section would
// mean rewriting three platform variants of upb's macro block. Revisit if
// microtel ever gains an extension of its own.

#ifndef MICROTEL_UPB_RENAME_H_
#define MICROTEL_UPB_RENAME_H_

#define UPB_linkarr_internal_empty_upb_AllExts microtel_UPB_linkarr_internal_empty_upb_AllExts
#define _kUpb_FromBase92 microtel__kUpb_FromBase92
#define _kUpb_MiniTable_Empty_dont_copy_me__upb_internal_use_only microtel__kUpb_MiniTable_Empty_dont_copy_me__upb_internal_use_only
#define _kUpb_MiniTable_StaticallyTreeShaken_dont_copy_me__upb_internal_use_only microtel__kUpb_MiniTable_StaticallyTreeShaken_dont_copy_me__upb_internal_use_only
#define _kUpb_ToBase92 microtel__kUpb_ToBase92
#define _upb_ArenaHas_dont_copy_me__upb_internal_use_only microtel__upb_ArenaHas_dont_copy_me__upb_internal_use_only
#define _upb_Arena_Contains_dont_copy_me__upb_internal_use_only microtel__upb_Arena_Contains_dont_copy_me__upb_internal_use_only
#define _upb_Arena_SlowMalloc_dont_copy_me__upb_internal_use_only microtel__upb_Arena_SlowMalloc_dont_copy_me__upb_internal_use_only
#define _upb_Arena_SwapIn_dont_copy_me__upb_internal_use_only microtel__upb_Arena_SwapIn_dont_copy_me__upb_internal_use_only
#define _upb_Arena_SwapOut_dont_copy_me__upb_internal_use_only microtel__upb_Arena_SwapOut_dont_copy_me__upb_internal_use_only
#define _upb_Array_ElemSizeLg2_dont_copy_me__upb_internal_use_only microtel__upb_Array_ElemSizeLg2_dont_copy_me__upb_internal_use_only
#define _upb_Array_New_dont_copy_me__upb_internal_use_only microtel__upb_Array_New_dont_copy_me__upb_internal_use_only
#define _upb_Array_Realloc_dont_copy_me__upb_internal_use_only microtel__upb_Array_Realloc_dont_copy_me__upb_internal_use_only
#define _upb_Array_ResizeUninitialized_dont_copy_me__upb_internal_use_only microtel__upb_Array_ResizeUninitialized_dont_copy_me__upb_internal_use_only
#define _upb_Array_SetTaggedPtr_dont_copy_me__upb_internal_use_only microtel__upb_Array_SetTaggedPtr_dont_copy_me__upb_internal_use_only
#define _upb_Array_Set_dont_copy_me__upb_internal_use_only microtel__upb_Array_Set_dont_copy_me__upb_internal_use_only
#define _upb_Decoder_CheckRequired microtel__upb_Decoder_CheckRequired
#define _upb_Decoder_IsDoneFallback microtel__upb_Decoder_IsDoneFallback
#define _upb_EpsCopyInputStream_IsDoneFallbackNoCallback microtel__upb_EpsCopyInputStream_IsDoneFallbackNoCallback
#define _upb_FastDecoder_DecodeGeneric microtel__upb_FastDecoder_DecodeGeneric
#define _upb_FastDecoder_ErrorJmp microtel__upb_FastDecoder_ErrorJmp
#define _upb_FieldType_SizeLg2_dont_copy_me__upb_internal_use_only microtel__upb_FieldType_SizeLg2_dont_copy_me__upb_internal_use_only
#define _upb_Hash microtel__upb_Hash
#define _upb_Map_CTypeSizeTable microtel__upb_Map_CTypeSizeTable
#define _upb_Map_New microtel__upb_Map_New
#define _upb_Message_AddUnknown_dont_copy_me__upb_internal_use_only microtel__upb_Message_AddUnknown_dont_copy_me__upb_internal_use_only
#define _upb_Message_Copy microtel__upb_Message_Copy
#define _upb_Message_DataPtr_dont_copy_me__upb_internal_use_only microtel__upb_Message_DataPtr_dont_copy_me__upb_internal_use_only
#define _upb_Message_DiscardUnknown_shallow microtel__upb_Message_DiscardUnknown_shallow
#define _upb_Message_GetHasbit_dont_copy_me__upb_internal_use_only microtel__upb_Message_GetHasbit_dont_copy_me__upb_internal_use_only
#define _upb_Message_GetOneofCase_dont_copy_me__upb_internal_use_only microtel__upb_Message_GetOneofCase_dont_copy_me__upb_internal_use_only
#define _upb_Message_GetOrCreateExtension_dont_copy_me__upb_internal_use_only microtel__upb_Message_GetOrCreateExtension_dont_copy_me__upb_internal_use_only
#define _upb_Message_Getext_dont_copy_me__upb_internal_use_only microtel__upb_Message_Getext_dont_copy_me__upb_internal_use_only
#define _upb_Message_Getexts_dont_copy_me__upb_internal_use_only microtel__upb_Message_Getexts_dont_copy_me__upb_internal_use_only
#define _upb_Message_MutableDataPtr_dont_copy_me__upb_internal_use_only microtel__upb_Message_MutableDataPtr_dont_copy_me__upb_internal_use_only
#define _upb_Message_New microtel__upb_Message_New
#define _upb_Message_NextBaseField_dont_copy_me__upb_internal_use_only microtel__upb_Message_NextBaseField_dont_copy_me__upb_internal_use_only
#define _upb_Message_NextExtension_dont_copy_me__upb_internal_use_only microtel__upb_Message_NextExtension_dont_copy_me__upb_internal_use_only
#define _upb_Message_OneofCasePtr_dont_copy_me__upb_internal_use_only microtel__upb_Message_OneofCasePtr_dont_copy_me__upb_internal_use_only
#define _upb_Message_Realloc_dont_copy_me__upb_internal_use_only microtel__upb_Message_Realloc_dont_copy_me__upb_internal_use_only
#define _upb_Message_SetField_dont_copy_me__upb_internal_use_only microtel__upb_Message_SetField_dont_copy_me__upb_internal_use_only
#define _upb_Message_SetHasbit_dont_copy_me__upb_internal_use_only microtel__upb_Message_SetHasbit_dont_copy_me__upb_internal_use_only
#define _upb_Message_SetOneofCase_dont_copy_me__upb_internal_use_only microtel__upb_Message_SetOneofCase_dont_copy_me__upb_internal_use_only
#define _upb_Message_SetPresence_dont_copy_me__upb_internal_use_only microtel__upb_Message_SetPresence_dont_copy_me__upb_internal_use_only
#define _upb_Message_UnknownFieldsAreEqual_dont_copy_me__upb_internal_use_only microtel__upb_Message_UnknownFieldsAreEqual_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableExtension_Build microtel__upb_MiniTableExtension_Build
#define _upb_MiniTableExtension_Init microtel__upb_MiniTableExtension_Init
#define _upb_MiniTableField_CheckIsArray_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_CheckIsArray_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_DataCopy_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_DataCopy_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_DataEquals_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_DataEquals_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_DataIsZero_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_DataIsZero_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_ElemSizeLg2_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_ElemSizeLg2_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_GetRep_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_GetRep_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_HasHasbit_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_HasHasbit_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_HasbitMask_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_HasbitMask_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_HasbitOffset_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_HasbitOffset_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_IsAlternate_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_IsAlternate_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_Mode_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_Mode_dont_copy_me__upb_internal_use_only
#define _upb_MiniTableField_OneofOffset_dont_copy_me__upb_internal_use_only microtel__upb_MiniTableField_OneofOffset_dont_copy_me__upb_internal_use_only
#define _upb_MiniTable_Build microtel__upb_MiniTable_Build
#define _upb_MiniTable_StrongReference_dont_copy_me__upb_internal_use_only microtel__upb_MiniTable_StrongReference_dont_copy_me__upb_internal_use_only
#define _upb_WireReader_ReadLongVarint_dont_copy_me__upb_internal_use_only microtel__upb_WireReader_ReadLongVarint_dont_copy_me__upb_internal_use_only
#define _upb_WireReader_SkipGroup_dont_copy_me__upb_internal_use_only microtel__upb_WireReader_SkipGroup_dont_copy_me__upb_internal_use_only
#define _upb_mapsorter_pushexts microtel__upb_mapsorter_pushexts
#define _upb_mapsorter_pushmap microtel__upb_mapsorter_pushmap
#define kUpb_FltInfinity microtel_kUpb_FltInfinity
#define kUpb_Infinity microtel_kUpb_Infinity
#define kUpb_NaN microtel_kUpb_NaN
#define kWyhashSalt microtel_kWyhashSalt
#define upb_Arena_DebugRefCount microtel_upb_Arena_DebugRefCount
#define upb_Arena_DecRefFor microtel_upb_Arena_DecRefFor
#define upb_Arena_Free microtel_upb_Arena_Free
#define upb_Arena_Fuse microtel_upb_Arena_Fuse
#define upb_Arena_IncRefFor microtel_upb_Arena_IncRefFor
#define upb_Arena_Init microtel_upb_Arena_Init
#define upb_Arena_Malloc microtel_upb_Arena_Malloc
#define upb_Arena_New microtel_upb_Arena_New
#define upb_Arena_SetMaxBlockSize microtel_upb_Arena_SetMaxBlockSize
#define upb_Arena_SpaceAllocated microtel_upb_Arena_SpaceAllocated
#define upb_Array_Append microtel_upb_Array_Append
#define upb_Array_DataPtr microtel_upb_Array_DataPtr
#define upb_Array_DeepClone microtel_upb_Array_DeepClone
#define upb_Array_Delete microtel_upb_Array_Delete
#define upb_Array_Freeze microtel_upb_Array_Freeze
#define upb_Array_Get microtel_upb_Array_Get
#define upb_Array_GetMutable microtel_upb_Array_GetMutable
#define upb_Array_Insert microtel_upb_Array_Insert
#define upb_Array_IsFrozen microtel_upb_Array_IsFrozen
#define upb_Array_Move microtel_upb_Array_Move
#define upb_Array_MutableDataPtr microtel_upb_Array_MutableDataPtr
#define upb_Array_New microtel_upb_Array_New
#define upb_Array_PromoteMessages microtel_upb_Array_PromoteMessages
#define upb_Array_Reserve microtel_upb_Array_Reserve
#define upb_Array_Resize microtel_upb_Array_Resize
#define upb_Array_Set microtel_upb_Array_Set
#define upb_ByteSize microtel_upb_ByteSize
#define upb_Decode microtel_upb_Decode
#define upb_DecodeLengthPrefixed microtel_upb_DecodeLengthPrefixed
#define upb_DecodeStatus_String microtel_upb_DecodeStatus_String
#define upb_Encode microtel_upb_Encode
#define upb_EncodeLengthPrefixed microtel_upb_EncodeLengthPrefixed
#define upb_EncodeStatus_String microtel_upb_EncodeStatus_String
#define upb_ExtensionRegistry_Add microtel_upb_ExtensionRegistry_Add
#define upb_ExtensionRegistry_AddAllLinkedExtensions microtel_upb_ExtensionRegistry_AddAllLinkedExtensions
#define upb_ExtensionRegistry_AddArray microtel_upb_ExtensionRegistry_AddArray
#define upb_ExtensionRegistry_Lookup microtel_upb_ExtensionRegistry_Lookup
#define upb_ExtensionRegistry_New microtel_upb_ExtensionRegistry_New
#define upb_MapIterator_Done microtel_upb_MapIterator_Done
#define upb_MapIterator_Key microtel_upb_MapIterator_Key
#define upb_MapIterator_Next microtel_upb_MapIterator_Next
#define upb_MapIterator_Value microtel_upb_MapIterator_Value
#define upb_Map_Clear microtel_upb_Map_Clear
#define upb_Map_DeepClone microtel_upb_Map_DeepClone
#define upb_Map_Delete microtel_upb_Map_Delete
#define upb_Map_Freeze microtel_upb_Map_Freeze
#define upb_Map_Get microtel_upb_Map_Get
#define upb_Map_Insert microtel_upb_Map_Insert
#define upb_Map_New microtel_upb_Map_New
#define upb_Map_Next microtel_upb_Map_Next
#define upb_Map_PromoteMessages microtel_upb_Map_PromoteMessages
#define upb_Map_SetEntryValue microtel_upb_Map_SetEntryValue
#define upb_Map_Size microtel_upb_Map_Size
#define upb_Message_DeepClone microtel_upb_Message_DeepClone
#define upb_Message_DeepCopy microtel_upb_Message_DeepCopy
#define upb_Message_DeleteUnknown microtel_upb_Message_DeleteUnknown
#define upb_Message_ExtensionByIndex microtel_upb_Message_ExtensionByIndex
#define upb_Message_ExtensionCount microtel_upb_Message_ExtensionCount
#define upb_Message_FindExtensionByNumber microtel_upb_Message_FindExtensionByNumber
#define upb_Message_FindUnknown microtel_upb_Message_FindUnknown
#define upb_Message_Freeze microtel_upb_Message_Freeze
#define upb_Message_GetArray microtel_upb_Message_GetArray
#define upb_Message_GetMutableArray microtel_upb_Message_GetMutableArray
#define upb_Message_GetOrCreateMutableArray microtel_upb_Message_GetOrCreateMutableArray
#define upb_Message_GetOrPromoteExtension microtel_upb_Message_GetOrPromoteExtension
#define upb_Message_GetUnknown microtel_upb_Message_GetUnknown
#define upb_Message_HasBaseField microtel_upb_Message_HasBaseField
#define upb_Message_IsEmpty microtel_upb_Message_IsEmpty
#define upb_Message_IsEqual microtel_upb_Message_IsEqual
#define upb_Message_IsFrozen microtel_upb_Message_IsFrozen
#define upb_Message_MergeFrom microtel_upb_Message_MergeFrom
#define upb_Message_New microtel_upb_Message_New
#define upb_Message_PromoteMessage microtel_upb_Message_PromoteMessage
#define upb_Message_SetBaseField microtel_upb_Message_SetBaseField
#define upb_Message_SetExtension microtel_upb_Message_SetExtension
#define upb_Message_SetMapEntry microtel_upb_Message_SetMapEntry
#define upb_Message_ShallowClone microtel_upb_Message_ShallowClone
#define upb_Message_ShallowCopy microtel_upb_Message_ShallowCopy
#define upb_MiniTableEnum_Build microtel_upb_MiniTableEnum_Build
#define upb_MiniTableField_HasPresence microtel_upb_MiniTableField_HasPresence
#define upb_MiniTableField_IsArray microtel_upb_MiniTableField_IsArray
#define upb_MiniTableField_IsExtension microtel_upb_MiniTableField_IsExtension
#define upb_MiniTableField_IsInOneof microtel_upb_MiniTableField_IsInOneof
#define upb_MiniTableField_IsScalar microtel_upb_MiniTableField_IsScalar
#define upb_MiniTableField_Number microtel_upb_MiniTableField_Number
#define upb_MiniTableField_Type microtel_upb_MiniTableField_Type
#define upb_MiniTable_BuildWithBuf microtel_upb_MiniTable_BuildWithBuf
#define upb_MiniTable_Compatible microtel_upb_MiniTable_Compatible
#define upb_MiniTable_Equals microtel_upb_MiniTable_Equals
#define upb_MiniTable_FindFieldByNumber microtel_upb_MiniTable_FindFieldByNumber
#define upb_MiniTable_GetOneof microtel_upb_MiniTable_GetOneof
#define upb_MiniTable_GetSubList microtel_upb_MiniTable_GetSubList
#define upb_MiniTable_Link microtel_upb_MiniTable_Link
#define upb_MiniTable_NextOneofField microtel_upb_MiniTable_NextOneofField
#define upb_MiniTable_PromoteUnknownToMap microtel_upb_MiniTable_PromoteUnknownToMap
#define upb_MiniTable_PromoteUnknownToMessage microtel_upb_MiniTable_PromoteUnknownToMessage
#define upb_MiniTable_PromoteUnknownToMessageArray microtel_upb_MiniTable_PromoteUnknownToMessageArray
#define upb_MiniTable_SetSubEnum microtel_upb_MiniTable_SetSubEnum
#define upb_MiniTable_SetSubMessage microtel_upb_MiniTable_SetSubMessage
#define upb_MtDataEncoder_EncodeExtension microtel_upb_MtDataEncoder_EncodeExtension
#define upb_MtDataEncoder_EncodeMap microtel_upb_MtDataEncoder_EncodeMap
#define upb_MtDataEncoder_EncodeMessageSet microtel_upb_MtDataEncoder_EncodeMessageSet
#define upb_MtDataEncoder_EndEnum microtel_upb_MtDataEncoder_EndEnum
#define upb_MtDataEncoder_PutEnumValue microtel_upb_MtDataEncoder_PutEnumValue
#define upb_MtDataEncoder_PutField microtel_upb_MtDataEncoder_PutField
#define upb_MtDataEncoder_PutModifier microtel_upb_MtDataEncoder_PutModifier
#define upb_MtDataEncoder_PutOneofField microtel_upb_MtDataEncoder_PutOneofField
#define upb_MtDataEncoder_StartEnum microtel_upb_MtDataEncoder_StartEnum
#define upb_MtDataEncoder_StartMessage microtel_upb_MtDataEncoder_StartMessage
#define upb_MtDataEncoder_StartOneof microtel_upb_MtDataEncoder_StartOneof
#define upb_Status_Clear microtel_upb_Status_Clear
#define upb_Status_ErrorMessage microtel_upb_Status_ErrorMessage
#define upb_Status_IsOk microtel_upb_Status_IsOk
#define upb_Status_SetErrorFormat microtel_upb_Status_SetErrorFormat
#define upb_Status_SetErrorMessage microtel_upb_Status_SetErrorMessage
#define upb_Status_VAppendErrorFormat microtel_upb_Status_VAppendErrorFormat
#define upb_Status_VSetErrorFormat microtel_upb_Status_VSetErrorFormat
#define upb_StringView_FromDataAndSize microtel_upb_StringView_FromDataAndSize
#define upb_StringView_IsEqual microtel_upb_StringView_IsEqual
#define upb_alloc_global microtel_upb_alloc_global
#define upb_inttable_compact microtel_upb_inttable_compact
#define upb_inttable_count microtel_upb_inttable_count
#define upb_inttable_init microtel_upb_inttable_init
#define upb_inttable_insert microtel_upb_inttable_insert
#define upb_inttable_lookup microtel_upb_inttable_lookup
#define upb_inttable_next microtel_upb_inttable_next
#define upb_inttable_remove microtel_upb_inttable_remove
#define upb_inttable_removeiter microtel_upb_inttable_removeiter
#define upb_inttable_replace microtel_upb_inttable_replace
#define upb_inttable_sizedinit microtel_upb_inttable_sizedinit
#define upb_strtable_begin microtel_upb_strtable_begin
#define upb_strtable_clear microtel_upb_strtable_clear
#define upb_strtable_done microtel_upb_strtable_done
#define upb_strtable_init microtel_upb_strtable_init
#define upb_strtable_insert microtel_upb_strtable_insert
#define upb_strtable_iter_isequal microtel_upb_strtable_iter_isequal
#define upb_strtable_iter_key microtel_upb_strtable_iter_key
#define upb_strtable_iter_setdone microtel_upb_strtable_iter_setdone
#define upb_strtable_iter_value microtel_upb_strtable_iter_value
#define upb_strtable_lookup2 microtel_upb_strtable_lookup2
#define upb_strtable_next microtel_upb_strtable_next
#define upb_strtable_next2 microtel_upb_strtable_next2
#define upb_strtable_remove2 microtel_upb_strtable_remove2
#define upb_strtable_removeiter microtel_upb_strtable_removeiter
#define upb_strtable_resize microtel_upb_strtable_resize
#define upb_strtable_setentryvalue microtel_upb_strtable_setentryvalue
#define utf8_range_IsValid microtel_utf8_range_IsValid
#define utf8_range_ValidPrefix microtel_utf8_range_ValidPrefix

#endif  // MICROTEL_UPB_RENAME_H_
