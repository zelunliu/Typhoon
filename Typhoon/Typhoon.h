/**
 * @file  Typhoon.h
 * @brief core header of Typhoon slice-scrambled LSD radix sort algorithm
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

#define MAX_PRINTOUT	1024
#define PRINT(fmt, ...) { char buf_PRINT[MAX_PRINTOUT] = "%s: "; strcat_s(buf_PRINT, MAX_PRINTOUT, fmt); printf (buf_PRINT, __FUNCTION__, ##__VA_ARGS__); }
#define ReportError(fmt, ...) { PRINT(fmt, ##__VA_ARGS__); \
								exit(-1); }

typedef unsigned __int64 uint64;
typedef __int64 int64;

#pragma pack(push,1)
struct DumpRecord {
	BYTE idx;			// bucket index
	void* ptr;			// Slice pointer (dynamically type-casted at runtime)
};
#pragma pack(pop)

#include "asm.h"
#include "config.h"

// Note: Values are dynamically initialized inside MultiThreadedLSD constructor based on 32-bit vs 64-bit modes
// 32-bit or 64-bit full sorts: digitStart = 0; 32+32 KV pairs: digitStart = 4
// constructing KV pairs: uint64 item = (key << 32) + value

class Barrier {
	volatile LONG reached;									// # of threads at the barrier
	CONDITION_VARIABLE cv;
	CRITICAL_SECTION cs;
public:

	Barrier()
	{
		InitializeConditionVariable(&cv);
		InitializeCriticalSection(&cs);
		reached = 0;
	}

	~Barrier() { DeleteCriticalSection(&cs); }

	bool WaitForAll(int nThreads, bool wake)
	{
		EnterCriticalSection(&cs);
		if (++reached < nThreads)
		{
			SleepConditionVariableCS(&cv, &cs, INFINITE);
			LeaveCriticalSection(&cs);
			return false;
		}
		else
		{
			reached = 0;
			if (wake) WakeEverybody();
			LeaveCriticalSection(&cs);
			return true;			// last thread
		}
	}

	void WakeEverybody(void)
	{
		WakeAllConditionVariable(&cv);
	}

	void Reset(void) { reached = 0; }
};

struct ThreadState {
	void* pNext[2][256];                    // Replaced KeyType* with type-erased void*
	uint64_t* cnt;
	uint64_t a[2][256], z[2][256];
	uint64 idx[256];
	void* tmpBuckets;                       // must be CACHE_LINE*elementSize aligned
	void** stmp;                            // WCv2 signature type-erased
	USHORT* firstVisitStagger;
	int threadID;
	void** freeStackStart, ** freeStackEnd; // Dynamic stack pointers
	void** sliceDatabase[2][256], ** sliceDatabaseStart[2][256];
	void** sdBuffer[2];
	DumpRecord* sliceDump;                  // all slices from a level
	void* aux;
	void** ins;                             // L0 input slices
	uint64_t start;                         // where to start the sort

	LARGE_INTEGER startSort, finishSort;
};

struct Job {
	int buck, thread;
	uint64_t slice;
};

class Typhoon;
struct Params {
	int threadID;
	Typhoon* mlsd;
};

class Typhoon {
	void* input;                            // Extracted KeyType dependency
	void* copyInput;
	UINT64 len;
	Barrier br;
	HANDLE* threads;
	Params* p;
	uint64 wcLine, sliceSizeKeys, sliceSizeKeyPower, wcLineBytes;
	uint64 pageSizeKeyPower;
	USHORT cstagger[256];                   // cache stagger
	uint64_t tmpBucketBytes;
	ThreadState* ts;
	uint64 nSlices, auxSlices, auxSlicesPerLevel;
	uint64 bitmapSize;
	uint64* pfn, * nextPfn;
	uint64 nPages, nPagesData, pageSizeBytes;
	LARGE_INTEGER freq, start, startSort, end;
	UINT64 totalMemory;                     // full array size with aux slices
	void** globalStackStart, ** globalStackEnd;
	UINT64 nSlicesPerThread, tmpSize;
	uint64 wcStaggerWastedSlices, slicesNeededPerLevel;
	HANDLE hV;
	int hstagger;
	int keyType;                           
	int digitStart;                         
	int digitCount;                         
	int keySizeByte;
	Config conf;

	SplitFuncL0Ptr L0;
	SplitFuncL1L2L4Ptr L1L2;
	SplitFuncL1L2L4Ptr L4;
	CleanupFuncPtr cleanup;
	CleanupL4FuncPtr cleanup_L4;
	HistFuncPtr L3;
	HistFuncSinglePtr L3_single;

	int nThreads, nCores;
	int level_stop;

	Typhoon(uint64_t nItems, int nThreads, int keyType, bool profile);
	void Init(uint64_t nItems, int nThreads, int keyType);
	void MakeIdx(void);
	void Reset();
	void PrefixSum(ThreadState* t, int level);
	Job ComputeJobStart(int src, uint64_t jobStart);
	void RunL1L2(int src, int digit, ThreadState* t, int level);
	void RunL3L4(int src, int digit, ThreadState* t);
	void PrefixSumSlices(ThreadState* t, int level);
	Job ComputeSliceStart(int src, uint64_t jobStart);
	void RunL5(int src, ThreadState* t);
	void GrantLockPagePrivilege(BOOL enable);
	void ReturnExtraSlices(ThreadState* t, uint64 threshold);
	void RefillSlices(ThreadState* t, uint64 threshold);
	void SetupDatabaseFromDump(ThreadState* t, int src, DumpRecord* end);
	void SetupDatabaseFromHistogram(ThreadState* t, int src);

public:
	int wcLineBits;
	int simd;
	int pref;
	uint64 sliceSizeBytes;
	uint64 elapsed[6];

	Typhoon(uint64_t nItems, int nThreads, int keyType);
	~Typhoon();
	void Profile();
	void RunAllThreads(int level_stop);
	void SortThread(int threadID);
	void* GetInputBuffer(void);             // Returns runtime adaptive void* address block
};