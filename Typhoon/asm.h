/**
 * @file  asm.h
 * @brief assembly declarations and linker interfaces
 *
 * @author Zelun Liu (Texas A&M University)
 * @author Arif Arman (Texas A&M University)
 * @author Dmitri Loguinov (Texas A&M University)
 *
 * Copyright (C) 2025 - 2026 Zelun Liu, Arif Arman, and Dmitri Loguinov.
 * All rights reserved.
 *
 * The 3-clause BSD License is applied to this software, see
 * license.txt
 */

#pragma once

#pragma pack(push,1)
struct AsmParams2 {
    void** ins;				// input slices (erased pointer array)
    void** pNext;			// pNext pointers
    uint64 digit;			// which digit to use
    void** stmp;			// tmp buckets
    void** freeStack;		// stack pointer
    DumpRecord* sdDump;		// record slices here
};

struct AsmParams3 {
    void* start;            // start of input (erased pointer)
    void* end;              // end of input (erased pointer)
    void** pNext;
    uint64 digit;
    void** stmp;
    void** freeStack;
    DumpRecord* sdDump;
};

struct DisjointSpace {
    void* start;            // erased pointer
    void* end;              // erased pointer
    bool operator< (const DisjointSpace& x) const {
        return start < x.start;
    }
};
#pragma pack(pop)

extern "C" int asm_get_slice_size(void);

// ============================================================================
// Function Pointer Typedefs
// ============================================================================

// L0 uses AsmParams3
typedef void** (*SplitFuncL0Ptr)(AsmParams3*);

// L1L2 and L4 use AsmParams2
typedef void** (*SplitFuncL1L2L4Ptr)(AsmParams2*);

// Cleanup variants
typedef void** (*CleanupFuncPtr)(void** pNext, void** stmp, void** freeStack, DumpRecord** sdDump);
typedef void** (*CleanupL4FuncPtr)(void** pNext, void** stmp, void** freeStack, void*** sliceDatabase);

// Histogram variants
typedef void (*HistFuncPtr)(void** slices, UINT64* hist);
typedef void (*HistFuncSinglePtr)(DisjointSpace* slices, UINT64* hist, UINT64 slicesInJob);

// ============================================================================
// Macros Declarations
// ============================================================================

#define SPLIT_NAME(func, bits, wc) asm_ ## func ## _uint ## bits ## _WC ## wc
#define HIST_NAME(func, bits, simd, pref) asm_ ## func ## _uint ## bits ## _SIMD ## simd ## _PREF ## pref

extern "C" {
    // Helper to declare Split variants (L0, L1L2, L4)
#define DECLARE_SPLIT_VARIANTS(func, bits, ParamType) \
        void** SPLIT_NAME(func, bits, 4)(ParamType*); \
        void** SPLIT_NAME(func, bits, 5)(ParamType*); \
        void** SPLIT_NAME(func, bits, 6)(ParamType*); \
        void** SPLIT_NAME(func, bits, 7)(ParamType*); \
        void** SPLIT_NAME(func, bits, 8)(ParamType*); \
        void** SPLIT_NAME(func, bits, 9)(ParamType*); \
        void** SPLIT_NAME(func, bits, 10)(ParamType*); \
        void** SPLIT_NAME(func, bits, 11)(ParamType*);

#define DECLARE_CLEANUP_VARIANTS(func, bits) \
        void** SPLIT_NAME(func, bits, 4)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 5)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 6)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 7)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 8)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 9)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 10)(void**, void**, void**, DumpRecord**); \
        void** SPLIT_NAME(func, bits, 11)(void**, void**, void**, DumpRecord**);

#define DECLARE_CLEANUP_L4_VARIANTS(func, bits) \
        void** SPLIT_NAME(func, bits, 4)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 5)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 6)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 7)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 8)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 9)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 10)(void**, void**, void**, void*** sliceDatabase); \
        void** SPLIT_NAME(func, bits, 11)(void**, void**, void**, void*** sliceDatabase);

    // Actual Declarations
    DECLARE_SPLIT_VARIANTS(L0, 32, AsmParams3)
    DECLARE_SPLIT_VARIANTS(L0, 64, AsmParams3)
    DECLARE_SPLIT_VARIANTS(L1L2, 32, AsmParams2)
    DECLARE_SPLIT_VARIANTS(L1L2, 64, AsmParams2)
    DECLARE_SPLIT_VARIANTS(L4, 32, AsmParams2)
    DECLARE_SPLIT_VARIANTS(L4, 64, AsmParams2)

    DECLARE_CLEANUP_VARIANTS(cleanup, 32)
    DECLARE_CLEANUP_VARIANTS(cleanup, 64)
    DECLARE_CLEANUP_L4_VARIANTS(cleanup_L4, 32)
    DECLARE_CLEANUP_L4_VARIANTS(cleanup_L4, 64)

    // Histogram Declarations
#define DECLARE_HIST_VARIANTS(func, bits) \
        void HIST_NAME(func, bits, 0, 0)(void**, UINT64*); \
        void HIST_NAME(func, bits, 0, 1)(void**, UINT64*); \
        void HIST_NAME(func, bits, 1, 0)(void**, UINT64*); \
        void HIST_NAME(func, bits, 1, 1)(void**, UINT64*);

#define DECLARE_HIST_SINGLE_VARIANTS(func, bits) \
        void HIST_NAME(func, bits, 0, 0)(DisjointSpace*, UINT64*, UINT64); \
        void HIST_NAME(func, bits, 0, 1)(DisjointSpace*, UINT64*, UINT64); \
        void HIST_NAME(func, bits, 1, 0)(DisjointSpace*, UINT64*, UINT64); \
        void HIST_NAME(func, bits, 1, 1)(DisjointSpace*, UINT64*, UINT64);

    DECLARE_HIST_VARIANTS(L3, 32)
    DECLARE_HIST_VARIANTS(L3, 64)
    DECLARE_HIST_SINGLE_VARIANTS(L3_single, 32)
    DECLARE_HIST_SINGLE_VARIANTS(L3_single, 64)
}

// ============================================================================
// Table Generation
// ============================================================================

#define SPLIT_ROW(func, bits) \
    { nullptr, nullptr, nullptr, nullptr, \
      SPLIT_NAME(func, bits, 4), SPLIT_NAME(func, bits, 5), SPLIT_NAME(func, bits, 6), \
      SPLIT_NAME(func, bits, 7), SPLIT_NAME(func, bits, 8), SPLIT_NAME(func, bits, 9), \
      SPLIT_NAME(func, bits, 10), SPLIT_NAME(func, bits, 11) }

#define HIST_ROW(func, bits) \
    { HIST_NAME(func, bits, 0, 0), HIST_NAME(func, bits, 0, 1), \
      HIST_NAME(func, bits, 1, 0), HIST_NAME(func, bits, 1, 1) }

// Use specific pointer types for each table
static constexpr SplitFuncL0Ptr SplitTable_L0[2][12] = {
    SPLIT_ROW(L0, 32), SPLIT_ROW(L0, 64)
};

static constexpr SplitFuncL1L2L4Ptr SplitTable_L1L2[2][12] = {
    SPLIT_ROW(L1L2, 32), SPLIT_ROW(L1L2, 64)
};

static constexpr SplitFuncL1L2L4Ptr SplitTable_L4[2][12] = {
    SPLIT_ROW(L4, 32), SPLIT_ROW(L4, 64)
};

static constexpr CleanupFuncPtr SplitTable_cleanup[2][12] = {
    SPLIT_ROW(cleanup, 32), SPLIT_ROW(cleanup, 64)
};

static constexpr CleanupL4FuncPtr SplitTable_cleanup_L4[2][12] = {
    SPLIT_ROW(cleanup_L4, 32), SPLIT_ROW(cleanup_L4, 64)
};

static constexpr HistFuncPtr HistTable_L3[2][4] = {
    HIST_ROW(L3, 32), HIST_ROW(L3, 64)
};

static constexpr HistFuncSinglePtr HistTable_L3_single[2][4] = {
    HIST_ROW(L3_single, 32), HIST_ROW(L3_single, 64)
};

// dynamic name modification macro configurations routing via runtime variable bit width
#define ASM_FUNC_HELPER(x, bits) ((this->keyType == 32) ? (x ## _32) : (x ## _64))
#define ASM_F(x) ASM_FUNC_HELPER(x, this->keyType)