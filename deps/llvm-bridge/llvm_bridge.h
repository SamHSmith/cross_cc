/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef CROSS_CC_LLVM_BRIDGE_HPP
#define CROSS_CC_LLVM_BRIDGE_HPP

#include <stdbool.h>
#include <stddef.h>
#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#ifdef __cplusplus
#define BRIDGE_EXTERN_C extern "C"
#else
#define BRIDGE_EXTERN_C
#endif

// ATTENTION: If you modify this file, be sure to update the corresponding
// extern function declarations in the self-hosted compiler.

// synchronize with llvm/include/Transforms/Instrumentation.h::SanitizerCoverageOptions::Type
// synchronize with codegen/llvm/bindings.zig::TargetMachine::EmitOptions::Coverage::Type
typedef enum BridgeLLVMCoverageType {
    BridgeLLVMCoverageType_None = 0,
    BridgeLLVMCoverageType_Function,
    BridgeLLVMCoverageType_BB,
    BridgeLLVMCoverageType_Edge
} BridgeLLVMCoverageType;

typedef struct BridgeLLVMCoverageOptions {
    BridgeLLVMCoverageType CoverageType;
    bool IndirectCalls;
    bool TraceBB;
    bool TraceCmp;
    bool TraceDiv;
    bool TraceGep;
    bool Use8bitCounters;
    bool TracePC;
    bool TracePCGuard;
    bool Inline8bitCounters;
    bool InlineBoolFlag;
    bool PCTable;
    bool NoPrune;
    bool StackDepth;
    bool TraceLoads;
    bool TraceStores;
    bool CollectControlFlow;
} BridgeLLVMCoverageOptions;

// synchronize with llvm/include/Pass.h::ThinOrFullLTOPhase
// synchronize with codegen/llvm/bindings.zig::EmitOptions::LtoPhase
typedef enum BridgeLLVMThinOrFullLTOPhase {
    BridgeLLVMThinOrFullLTOPhase_None,
    BridgeLLVMThinOrFullLTOPhase_ThinPreLink,
    BridgeLLVMThinOrFullLTOPhase_ThinkPostLink,
    BridgeLLVMThinOrFullLTOPhase_FullPreLink,
    BridgeLLVMThinOrFullLTOPhase_FullPostLink,
} BridgeLLVMThinOrFullLTOPhase;

typedef struct BridgeLLVMEmitOptions {
    bool is_debug;
    bool is_small;
    // If not null, and `BridgeLLVMTargetMachineEmitToFile` returns `false` indicating success, this
    // `char *` will be populated with a `malloc`-allocated string containing the serialized (as
    // JSON) time report data. The caller is responsible for freeing that memory.
    char **time_report_out;
    bool tsan;
    bool sancov;
    BridgeLLVMThinOrFullLTOPhase lto;
    bool allow_fast_isel;
    bool allow_machine_outliner;
    const char *asm_filename;
    const char *bin_filename;
    const char *llvm_ir_filename;
    const char *bitcode_filename;
    BridgeLLVMCoverageOptions coverage;
} BridgeLLVMEmitOptions;

// synchronize with llvm/include/Object/Archive.h::Object::Archive::Kind
// synchronize with codegen/llvm/bindings.zig::ArchiveKind
typedef enum BridgeLLVMArchiveKind {
    BridgeLLVMArchiveKind_GNU,
    BridgeLLVMArchiveKind_GNU64,
    BridgeLLVMArchiveKind_BSD,
    BridgeLLVMArchiveKind_DARWIN,
    BridgeLLVMArchiveKind_DARWIN64,
    BridgeLLVMArchiveKind_COFF,
    BridgeLLVMArchiveKind_AIXBIG,
} BridgeLLVMArchiveKind;

// synchronize with llvm/include/Target/TargetOptions.h::FloatABI::ABIType
// synchronize with codegen/llvm/bindings.zig::TargetMachine::FloatABI
typedef enum BridgeLLVMFloatABI {
    BridgeLLVMFloatABI_Default, // Target-specific (either soft or hard depending on triple, etc).
    BridgeLLVMFloatABI_Soft,    // Soft float.
    BridgeLLVMFloatABI_Hard     // Hard float.
} BridgeLLVMFloatABI;

BRIDGE_EXTERN_C bool BridgeLLVMTargetMachineEmitToFile(LLVMTargetMachineRef targ_machine_ref, LLVMModuleRef module_ref,
    char **error_message, const BridgeLLVMEmitOptions *options);

BRIDGE_EXTERN_C LLVMTargetMachineRef BridgeLLVMCreateTargetMachine(LLVMTargetRef T, const char *Triple,
    const char *CPU, const char *Features, LLVMCodeGenOptLevel Level, LLVMRelocMode Reloc,
    LLVMCodeModel CodeModel, bool function_sections, bool data_sections, BridgeLLVMFloatABI float_abi,
    const char *abi_name, bool emulated_tls);

BRIDGE_EXTERN_C void BridgeLLVMSetOptBisectLimit(LLVMContextRef context_ref, int limit);

BRIDGE_EXTERN_C void BridgeLLVMEnableBrokenDebugInfoCheck(LLVMContextRef context_ref);
BRIDGE_EXTERN_C bool BridgeLLVMGetBrokenDebugInfo(LLVMContextRef context_ref);

BRIDGE_EXTERN_C void BridgeLLVMParseCommandLineOptions(size_t argc, const char *const *argv);

BRIDGE_EXTERN_C bool BridgeLLDLinkCOFF(int argc, const char **argv, bool can_exit_early, bool disable_output);
BRIDGE_EXTERN_C bool BridgeLLDLinkELF(int argc, const char **argv, bool can_exit_early, bool disable_output);
BRIDGE_EXTERN_C bool BridgeLLDLinkWasm(int argc, const char **argv, bool can_exit_early, bool disable_output);
BRIDGE_EXTERN_C bool BridgeLLDLinkMachO(int argc, const char **argv, bool can_exit_early, bool disable_output);
BRIDGE_EXTERN_C bool BridgeLLDLinkMinGW(int argc, const char **argv, bool can_exit_early, bool disable_output);
BRIDGE_EXTERN_C int cross_cc_dsymutil_main(int argc, char **argv);

// On error, populates `*err_file_index_out` and `*err_msg_out` and returns `true`. The caller is
// responsible for freeing `*err_msg_out` using `free`.
//
// If an error occurs reading an input file, `*err_file_index_out` is set to the index of that input
// file in `file_names`. Otherwise, `*err_file_index_out` is set to `file_name_count`.
BRIDGE_EXTERN_C bool BridgeLLVMWriteArchive(const char *archive_name, const char **file_names, size_t file_name_count,
    BridgeLLVMArchiveKind archive_kind, size_t *err_file_index_out, char **err_msg_out);

#endif
