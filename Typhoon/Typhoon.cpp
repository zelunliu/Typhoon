/**
 * @file  Typhoon.cpp
 * @brief core implementation of Typhoon slice-scrambled LSD radix sort algorithm
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

#include "stdafx.h"
#include <intrin.h>
#include "Typhoon.h"

extern "C" {
	volatile void** min_stack;
}

Typhoon::Typhoon(uint64_t nItems, int nThreads, int keyType)
{
	conf.load("config.ini", keyType);
	if (conf.empty()) {
		Init((1 << 30) / (keyType / 8), 1, keyType);
		Profile();
	}
	Init(nItems, nThreads, keyType);

	// L3 flags are used to calculate the index for the HistTable
	int is64 = (this->keyType == 64);
	int l3_idx = (simd << 1) | pref; // Bit 1 = SIMD, Bit 0 = Prefetch

	// Assign the Function Pointers
	this->L0 = SplitTable_L0[is64][wcLineBits];
	this->L1L2 = SplitTable_L1L2[is64][wcLineBits];
	this->L4 = SplitTable_L4[is64][wcLineBits];
	this->cleanup = SplitTable_cleanup[is64][wcLineBits];
	this->cleanup_L4 = SplitTable_cleanup_L4[is64][wcLineBits];

	// Histograms use the SIMD/Prefetch combined index
	this->L3 = HistTable_L3[is64][l3_idx];
	this->L3_single = HistTable_L3_single[is64][l3_idx];
}

void Typhoon::Init(uint64_t nItems, int nThreads, int keyType) {
	this->keyType = keyType;
	this->keySizeByte = (keyType == 32) ? 4 : 8;
	this->digitStart = (keyType == 32) ? 0 : 4;
	this->digitCount = this->digitStart + 2;

	nCores = GetActiveProcessorCount(0);
	if (nThreads > nCores / 2) printf("Switching to hyperthreading...\n");

	wcLineBits = 11;
	if (!conf.empty()) {
		wcLineBits = conf["WC_LINE"];
		simd = conf["SIMD"];
		pref = conf["PrefetchT2"];
	}
	wcLine = 1LLU << wcLineBits;
	wcLineBytes = wcLine * this->keySizeByte;
	if (wcLineBytes < 64) { printf("wcLine smaller than cache line not supported\n"); exit(-1); }
	sliceSizeBytes = asm_get_slice_size();
	sliceSizeKeys = sliceSizeBytes / this->keySizeByte;
	sliceSizeKeyPower = (uint64)log2(sliceSizeKeys);			// old Xeons don't support lzcnt
	pageSizeBytes = 4096;
	pageSizeKeyPower = (uint64)log2(pageSizeBytes / this->keySizeByte);
	this->len = nItems;
	this->nThreads = nThreads;

	// NOTE: because of stagger, we waste wcLineBytes/2 bytes at the start of each level
	// Thus, we need up to wcLineBytes/2 * 256 / sliceSizeBytes extra slices (i.e., 8 for CACHE_LINE=6)
	// Since L4 needs 512 slices alone, the total consumption is [256 + wcLineBytes/2 * 256 + 512] slices per thread
	auxSlices = 256 * 3 + wcLineBytes / 2 * 256 / sliceSizeBytes;
	slicesNeededPerLevel = 256 + wcLineBytes / 2 * 256 / sliceSizeBytes;

	if ((globalStackStart = (void**)VirtualAlloc(NULL, auxSlices * nThreads * sizeof(void*),
		MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) == NULL) {
		printf("VirtualAlloc stack failed with %d\n", GetLastError()); exit(-1);
	}

	globalStackEnd = globalStackStart;

	// create memory [input buffer][padding][aux slices]
	UINT64 sizeBytes = nItems * this->keySizeByte;
	UINT64 roundedSizeBytes = (sizeBytes + sliceSizeBytes - 1) / sliceSizeBytes * sliceSizeBytes;
	nPagesData = roundedSizeBytes / pageSizeBytes;
	totalMemory = roundedSizeBytes + auxSlices * sliceSizeBytes * nThreads;

	// make sure physical pages come from the correct socket on Xeons
	SetThreadAffinityMask(GetCurrentThread(), 1);

	GrantLockPagePrivilege(true);
	if ((input = (void*)VirtualAlloc(NULL, totalMemory, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE)) == NULL) {
		printf("VirtualAlloc failed with %d\n", GetLastError()); exit(-1);
	}

	nPages = totalMemory / pageSizeBytes;			// totalMemory is slice aligned
	uint64 nRequested = nPages;
	pfn = new uint64[nPages];
	nextPfn = new uint64[nPages];	memset(nextPfn, 0, sizeof(uint64) * nPages);

	if (AllocateUserPhysicalPages(GetCurrentProcess(), &nPages, pfn) == FALSE) {
		printf("Allocate failed with %d\n", GetLastError()); exit(-1);
	}

	if (nPages < nRequested) {
		printf("Allocate obtained only %lld out of %lld pages\n", nPages, nRequested); exit(-1);
	}

	std::sort(pfn, pfn + nPages);

	if (MapUserPhysicalPages(input, nPages, pfn) == FALSE) {
		printf("Map failed with %d\n", GetLastError()); exit(-1);
	}

	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	ts = new ThreadState[nThreads];
	threads = new HANDLE[nThreads];
	p = new Params[nThreads];
	for (int i = 0; i < nThreads; i++)
	{
		p[i].threadID = nThreads - i - 1;
		p[i].mlsd = this;
	}

	nSlices = (sizeBytes + sliceSizeBytes - 1) / sliceSizeBytes;		// total slices needed across all threads
	nSlicesPerThread = (nSlices + nThreads - 1) / nThreads;				// upper bound on slices per threads

	hstagger = 8;							// stagger in bytes
	int nHistograms = 8;
	int extra = nHistograms * hstagger;		// extra bytes after the last histogram for the stagger
	
	// set up stagger for tmp buckets; ignore this many items from the front of each bucket
	// L1 cache-set idx = bits 6-11 in the virtual address; we need to ensure that each of the 64 cache-sets
	// gets hits equally (nBuckets/64 = 4 times)
	uint64 repeat = (1 << 12) / wcLineBytes;			// number of cache sets hit by no-stagger
	repeat = (repeat == 0) ? 1 : repeat;			// handle cases when wcLine is larger than page size
	uint64 cacheLineKeys = 64 / this->keySizeByte;
	for (int i = 0; i < 256; i++)
	{
		cstagger[i] = (USHORT)(((i / repeat) * cacheLineKeys) % wcLine);
	}
	// randomly shuffle the stagger array; must be done before the malloc for stmp since it uses cstagger[255]
	std::random_device seed;
	std::mt19937 generator(seed());
	std::shuffle(cstagger, cstagger + 256, generator);

	for (int j = 0; j < nThreads; j++)
	{
		ThreadState* t = ts + j;
		// ideal start of the job with [0, n-1]
		t->start = (j * nItems) / nThreads;
		if (j > 0 && t->start < ts[j - 1].start + sliceSizeKeys) {
			printf("Too many threads for the size of input. Should have at least one slice per thread.\n"); exit(-1);
		}

		// NOTE: align to 4K to make sure there is no false sharing
		t->cnt = (UINT64*)_aligned_malloc(256 * nHistograms * sizeof(UINT64) + extra, 4096);
		memset(t->cnt, 0, sizeof(UINT64) * 256 * nHistograms + extra);

		memset(t->pNext[0], 0, sizeof(void*) * 256);
		memset(t->pNext[1], 0, sizeof(void*) * 256);

		// layout of memory: [stmp array, firstVisitStagger, tmp buckets]
		tmpBucketBytes = this->keySizeByte * 256 * wcLine;

		// NOTE: tmpBuckets must be aligned to wcLineBytes
		uint64 sizeControl = sizeof(void*) * 256 + sizeof(*t->firstVisitStagger) * 256;
		sizeControl = (sizeControl + wcLineBytes - 1) & ~(wcLineBytes - 1);		// round up in case wcLine is huge

		t->stmp = (void**)_aligned_malloc(sizeControl + tmpBucketBytes, 4096); 
		t->firstVisitStagger = (USHORT*)((void**)t->stmp + 256);
		t->tmpBuckets = (void*)((char*)t->stmp + sizeControl);

		for (int i = 0; i < 256; i++)
		{
			t->stmp[i] = (char*)t->tmpBuckets + i * wcLine * this->keySizeByte + cstagger[i] * this->keySizeByte;
		}


		// reserve space for all slices, plus 2 at the end of each bucket (1 is partial, 1 contains the last slice size)
		t->sdBuffer[0] = (void**)_aligned_malloc((nSlicesPerThread + 2 * 256) * sizeof(void*), 4096);
		memset(t->sdBuffer[0], 0, (nSlicesPerThread + 2 * 256) * sizeof(void*));
		t->sdBuffer[1] = (void**)_aligned_malloc((nSlicesPerThread + 2 * 256) * sizeof(void*), 4096);
		memset(t->sdBuffer[1], 0, (nSlicesPerThread + 2 * 256) * sizeof(void*));
		// the dump contains all slices produced by a thread 
		t->sliceDump = (DumpRecord*)_aligned_malloc((nSlicesPerThread + 256) * sizeof(DumpRecord), 4096);
		memset(t->sliceDump, 0, sizeof(DumpRecord) * (nSlicesPerThread + 256));

		// We can spike the free stack beyond the initial size; for the worst case, allocate enough space to receive all 
		// slices across all threads
		t->freeStackStart = (void**)VirtualAlloc(NULL, auxSlices * nThreads * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (t->freeStackStart == NULL)
		{
			printf("thread %d: unable to allocate the stack with %d\n", j, GetLastError());
			exit(-1);
		}

		t->freeStackEnd = t->freeStackStart + auxSlices;
		//printf("free stack %p-%p\n", t->freeStackStart, t->freeStackEnd);

		t->aux = (char*)input + roundedSizeBytes + j * auxSlices * sliceSizeBytes;
		memset(t->aux, 0, auxSlices * sliceSizeBytes);

		for (int s = 0; s < auxSlices; s++)
			t->freeStackStart[s] = (char*)t->aux + s * sliceSizeBytes;
	}
}

void Typhoon::Profile() {
	sfmt_t s;
	sfmt_init_gen_rand(&s, 55);

	QueryPerformanceFrequency(&freq);

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	ThreadState* t = ts;

	for (int i = 0; i < 256; i++)
		t->firstVisitStagger[i] = cstagger[i] * this->keySizeByte;

	uint64 alignedStart = t->start / sliceSizeKeys * sliceSizeKeys;		// round down

	double best = -1;
	int is64 = (this->keyType == 64);

	printf("+--------------------------------------------------+\n");
	printf("| Evaluating Write-Combine Line Sizes              |\n");
	printf("+--------------------------------------------------+\n");
	
	int limit = (this->keyType == 32) ? 11 : 10;
	for (int i = 4; i <= limit; i++) {
		if (this->keySizeByte * (1 << i) > sliceSizeBytes) {
			break;
		}
		Reset();
		if (!is64) {
			uint32_t* input32 = static_cast<uint32_t*>(input);
			for (uint64_t idx = 0; idx < len; idx++)
				input32[idx] = (uint32_t)sfmt_genrand_uint32(&s);
		}
		else {
			uint64_t* input64 = static_cast<uint64_t*>(input);
			for (uint64_t idx = 0; idx < len; idx++)
				input64[idx] = (uint64_t)sfmt_genrand_uint64(&s);
		}

		uint64 repeat = (1 << 12) / (1LLU << i) * this->keySizeByte;	// number of cache sets hit by no-stagger
		repeat = (repeat == 0) ? 1 : repeat;			// handle cases when wcLine is larger than page size
		uint64 cacheLineKeys = 64 / this->keySizeByte;
		for (int k = 0; k < 256; k++)
		{
			cstagger[k] = (USHORT)(((k / repeat) * cacheLineKeys) % (1LLU << i));
		}

		// randomly shuffle the stagger array; must be done before the malloc for stmp since it uses cstagger[255]
		std::random_device seed;
		std::mt19937 generator(seed());
		std::shuffle(cstagger, cstagger + 256, generator);

		tmpBucketBytes = this->keySizeByte * 256 * (1LLU << i);
		uint64 sizeControl = sizeof(void*) * 256 + sizeof(*t->firstVisitStagger) * 256;
		sizeControl = (sizeControl + (1LLU << i) * this->keySizeByte - 1) & ~((1LLU << i) * this->keySizeByte - 1);		// round up in case wcLine is huge
		t->firstVisitStagger = (USHORT*)((void**)t->stmp + 256);
		t->tmpBuckets = (void*)((char*)t->stmp + sizeControl);
		for (int k = 0; k < 256; k++)
		{
			t->stmp[k] = (char*)t->tmpBuckets + k * (1LLU << i) * this->keySizeByte + cstagger[k] * this->keySizeByte;
		}

		AsmParams3 asmP = { (char*)input + alignedStart * this->keySizeByte, (char*)input + len / nThreads * this->keySizeByte, t->pNext[0], this->digitStart, t->stmp, t->freeStackEnd, t->sliceDump };
		L0 = SplitTable_L0[is64][i];
		cleanup = SplitTable_cleanup[is64][i];
		QueryPerformanceCounter(&start);
		t->freeStackEnd = L0(&asmP);
		t->freeStackEnd = cleanup(t->pNext[0], t->stmp, t->freeStackEnd, &asmP.sdDump);
		QueryPerformanceCounter(&end);

		double duration = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;

		// --- Specify what is being tested for L0 ---
		printf("|  WC_LINE = %-2d          -> Speed: %4.0f M/sec      |\n", i, len / duration / 1e6);
		if (len / duration / 1e6 > best) {
			conf["WC_LINE"] = i;
			best = len / duration / 1e6;
		}
	}
	printf("+--------------------------------------------------+\n");
	printf("|  * Selected WC_LINE: %-4d                        |\n", conf["WC_LINE"]);
	printf("+--------------------------------------------------+\n");
	wcLineBits = conf["WC_LINE"];
	if (is64) { Reset(); }

	// set up sliceDatabaseStart[0] & sliceDatabase[0] using the slice dump
	SetupDatabaseFromDump(t, 0, t->sliceDump);

	int temp = nThreads;
	nThreads = 1;
	printf("| Evaluating SIMD & Prefetching                    |\n");
	printf("+--------------------------------------------------+\n");
	best = -1;
	level_stop = 3;
	for (int i = 0; i <= 1; i++) {
		for (int j = 0; j <= 1; j++) {
			L3_single = HistTable_L3_single[is64][(i << 1) + j];
			QueryPerformanceCounter(&start);
			RunL3L4(0, this->digitStart + 3, t);
			QueryPerformanceCounter(&end);

			double duration = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;
			printf("|  SIMD = %d | PrefetchT2 = %d -> Speed: %4.0f M/sec  |\n", i, j, len / duration / 1e6);
			if (len / duration / 1e6 > best) {
				conf["SIMD"] = i;
				conf["PrefetchT2"] = j;
				best = len / duration / 1e6;
			}
		}
	}
	printf("+--------------------------------------------------+\n");
	printf("|  * Selected: SIMD = %d, PrefetchT2 = %d            |\n", conf["SIMD"], conf["PrefetchT2"]);
	printf("+--------------------------------------------------+\n\n");

	nThreads = temp;
	Reset();
	Reset();
	simd = conf["SIMD"];
	pref = conf["PrefetchT2"];
	conf.save("config.ini", this->keyType);
}

Typhoon::~Typhoon()
{
	for (int i = 0; i < nThreads; i++)
	{
		ThreadState* t = ts + i;
		_aligned_free(t->stmp);
		_aligned_free(t->cnt);
		VirtualFree(t->freeStackStart, 0, MEM_RELEASE);
	}

	delete[] ts;
	delete[] threads;
	delete[] p;
}

void Typhoon::Reset()
{
	br.Reset();						// barrier reset

	uint64* a = pfn;				// flip the PFN arrays
	pfn = nextPfn;
	nextPfn = a;

	for (int i = 0; i < nThreads; i++)
	{
		ThreadState* t = ts + i;

		memcpy(t->sliceDatabase[0], t->sliceDatabaseStart[0], sizeof(*t->sliceDatabaseStart[0]) * 256);
		memcpy(t->sliceDatabase[1], t->sliceDatabaseStart[1], sizeof(*t->sliceDatabaseStart[1]) * 256);

		memset(t->pNext[0], 0, sizeof(void*) * 256);
		memset(t->pNext[1], 0, sizeof(void*) * 256);

		memset(ts->sdBuffer[0], 0, (nSlicesPerThread + 2 * 256) * sizeof(void*));
		memset(ts->sdBuffer[1], 0, (nSlicesPerThread + 2 * 256) * sizeof(void*));

		memset(ts->sliceDump, 0, sizeof(DumpRecord) * (nSlicesPerThread + 256));

		for (int s = 0; s < auxSlices; s++)
			t->freeStackStart[s] = (char*)t->aux + s * sliceSizeBytes;
		t->freeStackEnd = t->freeStackStart + auxSlices;
		globalStackEnd = globalStackStart;

		for (int i = 0; i < 256; i++)
			t->stmp[i] = (char*)t->tmpBuckets + i * wcLine * this->keySizeByte + cstagger[i] * this->keySizeByte;

		int nHistograms = 8;
		int extra = nHistograms * hstagger;		// extra bytes after the last histogram for the stagger
		memset(t->cnt, 0, sizeof(UINT64) * 256 * nHistograms + extra);
	}
}


DWORD WINAPI TestRun(void* arg)
{
	Params* p = (Params*)arg;
	p->mlsd->SortThread(p->threadID);
	return 0;
}

void Typhoon::RunAllThreads(int level_stop)
{
	this->level_stop = level_stop;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	QueryPerformanceCounter(&startSort);
	QueryPerformanceCounter(&start);		// timing for L0

	for (int i = 0; i < nThreads; i++)
		threads[i] = CreateThread(NULL, 0, TestRun, p + i, 0, NULL);

	for (int i = 0; i < nThreads; i++)
	{
		int ret = WaitForSingleObject(threads[i], INFINITE);
		CloseHandle(threads[i]);

		if (i == nThreads - 1)			// make sure only 1 thread tries to reset
			Reset();
	}

	QueryPerformanceCounter(&end);
	elapsed[level_stop] = end.QuadPart - start.QuadPart;
}

void Typhoon::SortThread(int threadID)
{
	if (nThreads <= nCores / 2)
		SetThreadAffinityMask(GetCurrentThread(), 1LLU << (2 * threadID));
	else
		SetThreadAffinityMask(GetCurrentThread(), 1LLU << (threadID));

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	ThreadState* t = ts + threadID;
	t->threadID = threadID;

	// let ASM know what stagger we have to avoid copying empty buckets and wasting slices; this gets overwritten in L4
	for (int i = 0; i < 256; i++)
		t->firstVisitStagger[i] = cstagger[i] * this->keySizeByte;

	// level L0 ---------------------------------------
	// The L0 splitter assumes the start pointer is rounded to a slice, but the end pointer can be arbitrary
	uint64 alignedStart = t->start / sliceSizeKeys * sliceSizeKeys;		// round down
	uint64 end = threadID < nThreads - 1 ? ts[threadID + 1].start / sliceSizeKeys * sliceSizeKeys : len;

	AsmParams3 asmP = { (char*)input + alignedStart * this->keySizeByte, (char*)input + end * this->keySizeByte, t->pNext[0], this->digitStart, t->stmp, t->freeStackEnd, t->sliceDump };
	t->freeStackEnd = L0(&asmP);
	t->freeStackEnd = cleanup(t->pNext[0], t->stmp, t->freeStackEnd, &asmP.sdDump);

	SetupDatabaseFromDump(t, 0, asmP.sdDump);

	if (level_stop == 0) return;

	// Level L1 ---------------------------------------
	RunL1L2(0, this->digitStart + 1, t, 1);
	if (level_stop == 1) return;

	// Level L2 ---------------------------------------
	RunL1L2(1, this->digitStart + 2, t, 2);
	if (level_stop == 2) return;

	// Level L3/L4 ---------------------------------------
	RunL3L4(0, this->digitStart + 3, t);
	if (level_stop == 4) return;

	// Level L5 ---------------------------------------
	RunL5(1, t);
}

void Typhoon::MakeIdx(void)
{
	// complexity 256 * T
	uint64 q = 0;
	void* curSlice = NULL;
	uint64 mask = sliceSizeKeys - 1, notmask = ~mask, prevSlices = 0;
	uint64 src = 0, allocated = 0;
	int prev_i = 0, prev_th = 0;
	int nextThreadStack = 0;

	for (int i = 0; i < 256; i++)
	{
		for (int th = 0; th < nThreads; th++)
		{
			if (ts[th].cnt[i] == 0)
			{
				ts[th].pNext[src ^ 1][i] = NULL;		// give NULL to empty buckets to prevent cleanup from getting confused
				continue;
			}

			ts[th].idx[i] = q;						// bucket (i,th) begins
			uint64 tmp = q + ts[th].cnt[i];			// bucket (i,th) ends

			// NOTE1: there could be multiple buckets entirely contained in the same slice
			// NOTE2: there could be buckets with nothing in them (e.g., buck 5, thread 4 is followed by buck 7, thread 2)
			// NOTE3: it's possible that all shared slices are grabbed from one thread, depleting its supply of slices
			// All of these points are handled below

			// if the previous slice cannot be reused, get a new one
			if (curSlice == NULL)
			{
				if (globalStackEnd == globalStackStart)
				{
					printf("MakeIdx: globalStack underflow!!\n");
					exit(-1);
				}

				globalStackEnd--;
				curSlice = *globalStackEnd;						// preallocate the first slice
				allocated++;
			}

			// set the initial offset correctly within the slice
			void* ptr = (char*)curSlice + (q & mask) * this->keySizeByte;
			ts[th].pNext[src ^ 1][i] = NULL;
			ts[th].sliceDatabaseStart[src ^ 1][i][0] = ptr;

			// shared slice if q doesn't begin at a boundary; also, ignore cases when the previous bucket is < 2 slices long
			// if the prev bucket has size 1, we already gave it a slice
			if (prevSlices > 1 && (q & mask))
				ts[prev_th].sliceDatabaseStart[src ^ 1][prev_i][prevSlices - 1] = curSlice;		// start of slice

			// wipe the rest of the database of slices between [1, nSlices-1]
			uint64 nSlices = tmp - (q & notmask);										// round down the start
			nSlices = (nSlices + sliceSizeKeys - 1) >> sliceSizeKeyPower;				// round up to nearest slice
			memset(ts[th].sliceDatabaseStart[src ^ 1][i] + 1, 0, (nSlices - 1) * sizeof(void*));

			// if both start/end are contained in the same slice, keep curSlice; otherwise, reset
			if ((tmp & notmask) != (q & notmask))
				curSlice = NULL;

			q = tmp;
			prev_i = i;
			prev_th = th;
			prevSlices = nSlices;
		}
	}
}

void Typhoon::PrefixSum(ThreadState* t, int level)
{
	void*** st = t->sliceDatabaseStart[level], *** se = t->sliceDatabase[level];
	uint64 total = 0;

	for (int i = 0; i < 256; i++)
	{
		void** sliceBegin = st[i], ** sliceEnd = se[i];
		uint64 bucketSize = 0;

		if (sliceEnd > sliceBegin)			// bucket size is non-zero
		{
			bucketSize = (sliceEnd - sliceBegin - 1) << sliceSizeKeyPower;
			bucketSize += ((char*)t->pNext[level][i] - (char*)sliceEnd[-1]) / this->keySizeByte;
			bucketSize -= cstagger[i];
		}

		total += bucketSize;
		t->a[level][i] = bucketSize;
		t->z[level][i] = total;
	}
}

Job Typhoon::ComputeJobStart(int src, uint64_t jobStart)
{
#define JOB_BINARY_SEARCH
#ifdef JOB_BINARY_SEARCH
	// assume T threads
	// binary search: 0.2us for all threads with T=4, complexity log2(nBuckets)*T = 8T per thread
	// for T=64, the slowest thread takes < 3us 

	int left = 0, right = 255, mid = 128;
	uint64_t rowZ;
	while (left < right)
	{
		rowZ = 0;									// total keys in buckets [0, mid]
		for (int t = 0; t < nThreads; t++)
			rowZ += ts[t].z[src][mid];

		if (rowZ > jobStart)
			right = mid;
		else if (rowZ <= jobStart)
			left = mid + 1;
		mid = (left + right) >> 1;
	}

	// mid=left=right after the loop

	uint64_t prevRow = 0;			// get the sum for the previous row, then build up from there
	if (mid > 0)
	{
		for (int t = 0; t < nThreads; t++)
			prevRow += ts[t].z[src][mid - 1];
	}

	// find the starting thread and offset in that bucket
	for (int th = 0; th < nThreads; th++)
	{
		uint64_t s = prevRow + ts[th].a[src][mid];

		if (s > jobStart)
		{
			// tuple (bucket, thread, slice #)
			Job jb = { mid, th, (jobStart - prevRow + cstagger[mid]) >> sliceSizeKeyPower };
			return jb;
		}
		prevRow = s;
	}
#else
	// naive version: complexity = nBuckets * T per thread; for T=4 and 256 buckets, 0.7-0.8us for the last thread (< 0.1us for the first thread)
	// overall cost = nBuckets * T^2, but the delay is linear in T
	// For T=64, the largest delay goes to ~56us

	uint64_t obtained = 0;
	for (int i = 0; i < 256; i++)
	{
		for (int th = 0; th < nThreads; th++)
		{
			uint64_t buckSize = ts[th].a[src][i];
			uint64_t s = obtained + buckSize;
			if (s > jobStart)
			{
				// begin job in this bucket
				Job jb = { i, th, jobStart - obtained };
				return jb;
			}
			obtained = s;
		}
	}
#endif
	printf("Unable to find a job\n");
	exit(-1);
}

void Typhoon::RunL1L2(int src, int digit, ThreadState* t, int level)
{
	ReturnExtraSlices(t, slicesNeededPerLevel);

	// reset stagger
	for (int i = 0; i < 256; i++)
		t->stmp[i] = (char*)t->tmpBuckets + i * wcLine * this->keySizeByte + cstagger[i] * this->keySizeByte;

	// prefix sum of bucket sizes, t->a[src][i] = size of bucket i, t->z[src][i] = sum of all bucket sizes 0, .., i
	PrefixSum(t, src);
	if (br.WaitForAll(nThreads, true))
	{
		QueryPerformanceCounter(&end);
		elapsed[level - 1] = end.QuadPart - start.QuadPart;		// compute speed per level
		start = end;
	}

	RefillSlices(t, slicesNeededPerLevel);

	// reset pNext and slide database
	memset(t->pNext[src ^ 1], 0, sizeof(void*) * 256);

	// compute the job start for this thread
	Job jb = ComputeJobStart(src, t->start);
	Job endjb = { 256, 0, 0 };
	if (t->threadID < nThreads - 1)
		endjb = ComputeJobStart(src, ts[t->threadID + 1].start);			// where the next thread begins

	int initialThread = jb.thread;
	uint64_t slice = jb.slice;

	void** minst = NULL;
	bool keepGoing = true;
	DumpRecord* dumpPtr = t->sliceDump;
	for (int i = jb.buck; i < 256 && keepGoing; i++)
	{
		for (int th = initialThread; th < nThreads && keepGoing; th++)
		{
			void** sliceBegin = ts[th].sliceDatabaseStart[src][i] + slice, ** sliceEnd = ts[th].sliceDatabase[src][i];
			uint64 slicesInBucket = sliceEnd - sliceBegin;

			uint64 slicesToTake;
			if (i == endjb.buck && th == endjb.thread)
			{
				keepGoing = false;
				slicesToTake = endjb.slice - slice;
			}
			else
				slicesToTake = slicesInBucket;

			if (slicesToTake > 0)
			{
				// NOTE: we cannot treat both cases with |= 1 because this would require wiping the next slice pointer with size
				if (slicesToTake < slicesInBucket)
					*(uint64*)(sliceBegin + slicesToTake - 1) |= 3;			// last full slice
				else
				{
					// mark the last slice as partial
					*(uint64*)(sliceEnd - 1) |= 1;
					*sliceEnd = ts[th].pNext[src][i];
				}

				// adjust the first slice by stagger; NOTE: only 1 thread modifies this and others are unaffected by the change
				if (slice == 0)
					sliceBegin[0] = (char*)sliceBegin[0] + cstagger[i] * this->keySizeByte;

				AsmParams2 asmP = { sliceBegin, t->pNext[src ^ 1], (uint64)digit, t->stmp, t->freeStackEnd, dumpPtr };
				t->freeStackEnd = L1L2(&asmP);
				dumpPtr = asmP.sdDump;							// update after each bucket
			}
			slice = 0;
		}
		initialThread = 0;
	}

	t->freeStackEnd = cleanup(t->pNext[src ^ 1], t->stmp, t->freeStackEnd, &dumpPtr);

	SetupDatabaseFromDump(t, src ^ 1, dumpPtr);
}

void Typhoon::RunL3L4(int src, int digit, ThreadState* t)
{
	// L3 = histogram, L4 = split back to input
	// prefix sum of bucket sizes, a[i] = size of bucket i, z[i] = sum of all bucket sizes 0, .., i
	PrefixSum(t, src);
	ReturnExtraSlices(t, 256);			// only need 256 since there is no stagger on L4

	if (br.WaitForAll(nThreads, true))
	{
		QueryPerformanceCounter(&end);
		elapsed[2] = end.QuadPart - start.QuadPart;
		start = end;
	}

	Job jb = { 0, 0, 0 }, endjb = { 256, 0, 0 };
	if (nThreads > 1)
	{
		// compute the job start for this thread
		jb = ComputeJobStart(src, t->start);
		if (t->threadID < nThreads - 1)
			endjb = ComputeJobStart(src, ts[t->threadID + 1].start);			// where the next thread begins

		int initialThread = jb.thread;
		uint64_t slice = jb.slice;

		uint64 slicesInJob = 0, totalKeys = 0;
		bool keepGoing = true;
		for (int i = jb.buck; i < 256 && keepGoing; i++)
		{
			for (int th = initialThread; th < nThreads && keepGoing; th++)
			{
				void** sliceBegin = ts[th].sliceDatabaseStart[src][i] + slice, ** sliceEnd = ts[th].sliceDatabase[src][i];
				uint64 slicesInBucket = sliceEnd - sliceBegin;

				uint64 slicesToTake;
				if (i == endjb.buck && th == endjb.thread)
				{
					keepGoing = false;
					slicesToTake = endjb.slice - slice;
				}
				else
					slicesToTake = slicesInBucket;

				if (slicesToTake > 0)
				{
					if (slicesToTake < slicesInBucket)
						*(uint64*)(sliceBegin + slicesToTake - 1) |= 3;			// last full slice
					else
					{
						// mark the last slice as partial
						*(uint64*)(sliceEnd - 1) |= 1;
						*sliceEnd = ts[th].pNext[src][i];
					}

					// adjust the first slice by stagger
					if (slice == 0)
						sliceBegin[0] = (char*)sliceBegin[0] + cstagger[i] * this->keySizeByte;

					L3(sliceBegin, t->cnt);
				}
				slice = 0;
			}
			initialThread = 0;
		}
	}
	else
	{
		uint64 stackSize = t->freeStackEnd - t->freeStackStart + globalStackEnd - globalStackStart;
		DisjointSpace* empty = new DisjointSpace[256 * 2 + stackSize];
		DisjointSpace* runs = new DisjointSpace[256 * 2 + stackSize];
		uint64 k = 0, pos = 0, excluded = 0;
		uint64 mask = ~(sliceSizeBytes - 1), total = 0;

		// single-threaded histogram: sort partial and free slices, then count whatever is between
		for (int i = 0; i < 256; i++)
		{
			void** sliceBegin = t->sliceDatabaseStart[src][i], ** sliceEnd = t->sliceDatabase[src][i];
			uint64 slicesInBucket = sliceEnd - sliceBegin;

			if (slicesInBucket > 0)
			{
				// we can have 0, 1, 2 empty runs per bucket
				if (cstagger[i] > 0)
				{
					// handle the first slice being partial
					empty[k].start = sliceBegin[0];
					empty[k++].end = (char*)sliceBegin[0] + cstagger[i] * this->keySizeByte;
				}

				if ((uint64)t->pNext[src][i] & (sliceSizeBytes - 1))
				{
					// handle the last slice being partial
					empty[k].start = t->pNext[src][i];
					empty[k++].end = (void*)((uint64)((char*)t->pNext[src][i] + sliceSizeKeys * this->keySizeByte) & mask);
				}

				// mark the last slice as partial; needed for L4
				*(uint64*)(sliceEnd - 1) |= 1;
				*sliceEnd = t->pNext[src][i];

				// adjust the first slice by stagger
				sliceBegin[0] = (char*)sliceBegin[0] + cstagger[i] * this->keySizeByte;
			}
		}

		for (void** sp = t->freeStackStart; sp < t->freeStackEnd; sp++)
		{
			empty[k].start = *sp;
			empty[k++].end = (char*)*sp + sliceSizeKeys * this->keySizeByte;
		}

		for (void** sp = globalStackStart; sp < globalStackEnd; sp++)
		{
			empty[k].start = *sp;
			empty[k++].end = (char*)*sp + sliceSizeKeys * this->keySizeByte;
		}

		std::sort(empty, empty + k);

		// convert empty space into runs of keys
		void* begin = input;
		for (uint64 j = 0; j < k; j++)
		{
			// check for empty back-to-back slices and skip
			if (begin < empty[j].start)
			{
				// the range is [begin, extracted[j].start)
				runs[pos].start = begin;
				runs[pos++].end = empty[j].start;
				total += ((char*)empty[j].start - (char*)begin) / this->keySizeByte;
			}
			begin = empty[j].end;
		}

		// the last run at the end of buffer
		void* finish = (char*)input + totalMemory;
		if (begin < finish)
		{
			runs[pos].start = begin;
			runs[pos++].end = finish;
			total += ((char*)finish - (char*)begin) / this->keySizeByte;
			//printf("[%d] range [%p, %p], run %lld, total keys %lld\n", pos-1, begin, finish, finish - begin, total);
		}

		L3_single(runs, t->cnt, pos);
		delete[] empty;
		delete[] runs;
	}

	// note: this assumes 8-byte stagger
	uint64 off = hstagger / sizeof(uint64);		// stagger in bytes
	UINT64* p0 = t->cnt, * p1 = p0 + 256 + off, * p2 = p1 + 256 + off, * p3 = p2 + 256 + off,
		* p4 = p3 + 256 + off, * p5 = p4 + 256 + off, * p6 = p5 + 256 + off, * p7 = p6 + 256 + off;

	UINT64 total = 0;
	for (int i = 0; i < 256; i++)
	{
		t->cnt[i] += p1[i] + p2[i] + p3[i] + p4[i] + p5[i] + p6[i] + p7[i];
	}

	if (level_stop == 3)
		return;

	// set up sliceDatabase preemptively based on the counters
	SetupDatabaseFromHistogram(t, src ^ 1);

	// L4 = split back to input -----------------------------------------------------------------------
	if (br.WaitForAll(nThreads, false))
	{
		// obtain destination pointers from the histograms for all T threads
		MakeIdx();
		br.WakeEverybody();

		QueryPerformanceCounter(&end);
		elapsed[3] = end.QuadPart - start.QuadPart;
		start = end;
	}

	RefillSlices(t, 256);		// only need 256 since there is no stagger on L4

	for (int i = 0; i < 256; i++)
	{
		t->stmp[i] = (char*)t->tmpBuckets + i * wcLine * this->keySizeByte + cstagger[i] * this->keySizeByte;

		// NOTE: small buckets (smaller than wcLineKeys - cstagger[i]) could be skipped, but make sure to set their firstVisitStagger
		uint64_t x = t->idx[i] * this->keySizeByte & 63;		// in bytes
		x /= this->keySizeByte;						// in keys
		t->firstVisitStagger[i] = (USHORT)(cstagger[i] + x) * this->keySizeByte;
		t->stmp[i] = (char*)t->stmp[i] + x * this->keySizeByte;
	}

	int initialThread = jb.thread;
	uint64 slice = jb.slice;
	bool keepGoing = true;

	void** minst = NULL;
	for (int i = jb.buck; i < 256 && keepGoing; i++)
	{
		for (int th = initialThread; th < nThreads && keepGoing; th++)
		{
			void** sliceBegin = ts[th].sliceDatabaseStart[src][i] + slice, ** sliceEnd = ts[th].sliceDatabase[src][i];
			uint64 slicesInBucket = sliceEnd - sliceBegin;

			uint64 slicesToTake;
			if (i == endjb.buck && th == endjb.thread)
			{
				keepGoing = false;
				slicesToTake = endjb.slice - slice;
			}
			else
				slicesToTake = slicesInBucket;

			if (slicesToTake > 0)
			{
				// NOTE: firstVisitStagger is passed implicitly as a buffer that follows stmp
				AsmParams2 asmP = { sliceBegin, t->pNext[src ^ 1], (uint64)digit, t->stmp, t->freeStackEnd, (DumpRecord*)t->sliceDatabase[src ^ 1] };
				t->freeStackEnd = L4(&asmP);
			}
			slice = 0;
		}
		initialThread = 0;
	}

	t->freeStackEnd = cleanup_L4(t->pNext[src ^ 1], t->stmp, t->freeStackEnd, t->sliceDatabase[src ^ 1]);

}

void* Typhoon::GetInputBuffer(void)
{
	return input;
}

// grants the privilege for the application to map/unmap in userspace
void Typhoon::GrantLockPagePrivilege(BOOL enable)
{
	TOKEN_PRIVILEGES tokenPrivileges;
	HANDLE           tokenHandle;

	// Open token
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tokenHandle))
		ReportError("OpenProcessToken Error %d\n", GetLastError());

	// enable
	tokenPrivileges.PrivilegeCount = 1;
	if (enable)
		tokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	else
		tokenPrivileges.Privileges[0].Attributes = 0;

	// get LUID
	if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &(tokenPrivileges.Privileges[0].Luid)))
		ReportError("LookupPrivilegeValue Error %d\n", GetLastError());

	// Elevate privilege of the process token
	if (!AdjustTokenPrivileges(tokenHandle, FALSE, &tokenPrivileges, 0, NULL, NULL))
		ReportError("AdjustTokenPrivileges Error %d\n", GetLastError());

	if (GetLastError() != ERROR_SUCCESS)
		PRINT("Cannot enable the SE_LOCK_MEMORY_NAME privilege; please check the local policy\n");

	if (!CloseHandle(tokenHandle))
		ReportError("CloseHandle Error %d\n", GetLastError());
}

void Typhoon::PrefixSumSlices(ThreadState* t, int level)
{
	uint64 mask = sliceSizeBytes - 1;
	void*** st = t->sliceDatabaseStart[level], *** se = t->sliceDatabase[level];
	uint64 total = 0;

	for (int i = 0; i < 256; i++)
	{
		void** sliceBegin = st[i], ** sliceEnd = se[i];
		uint64 slicesInBucket = sliceEnd - sliceBegin;
		uint64 count = 0;

		for (int j = 0; j < slicesInBucket; j++)
		{
			// only count pointers that begin a slice
			if (((uint64)sliceBegin[j] & mask) == 0)
				count++;
		}

		total += count;
		t->a[level][i] = count;
		t->z[level][i] = total;
	}
}

Job Typhoon::ComputeSliceStart(int src, uint64_t jobStart)
{
	int left = 0, right = 255, mid = 128;
	uint64_t rowZ;
	while (left < right)
	{
		rowZ = 0;									// total keys in buckets [0, mid]
		for (int t = 0; t < nThreads; t++)
			rowZ += ts[t].z[src][mid];

		if (rowZ > jobStart)
			right = mid;
		else if (rowZ <= jobStart)
			left = mid + 1;
		mid = (left + right) >> 1;
	}

	// mid=left=right after the loop

	uint64_t prevRow = 0;			// get the sum for the previous row, then build up from there
	if (mid > 0)
	{
		for (int t = 0; t < nThreads; t++)
			prevRow += ts[t].z[src][mid - 1];
	}

	// find the starting thread and offset in that bucket
	for (int th = 0; th < nThreads; th++)
	{
		uint64_t s = prevRow + ts[th].a[src][mid];

		if (s > jobStart)
		{
			uint64 sliceStart = jobStart - prevRow;
			void** sliceBegin = ts[th].sliceDatabaseStart[src][mid];
			// if the first slice is partial, take one extra
			if ((uint64)sliceBegin[0] & (sliceSizeBytes - 1))
				sliceStart++;

			// tuple (bucket, thread, slice #)
			Job jb = { mid, th,  sliceStart };

			return jb;
		}
		prevRow = s;
	}
	printf("Failed to obtain a job\n");
	exit(-1);
}

void Typhoon::RunL5(int src, ThreadState* t)
{
	//printf("-------------- L5a --------------------\n");
	PrefixSumSlices(t, src);
	if (br.WaitForAll(nThreads, true))			// wait for L4 to be done
	{
		QueryPerformanceCounter(&end);
		elapsed[4] = end.QuadPart - start.QuadPart;
		start = end;
	}

	uint64 pagesPerThread = nPages / nThreads;
	uint64 unmapLoad = (t->threadID == nThreads - 1) ? nPages - pagesPerThread * (nThreads - 1) : pagesPerThread;

	uint64 off = t->threadID * pagesPerThread;
	if (MapUserPhysicalPages((char*)input + off * pageSizeBytes, unmapLoad, NULL) == FALSE)
	{
		printf("Unmap failed with %d\n", GetLastError());
		exit(-1);
	}

	// compute the job start for this thread
	uint64 slicesPerThread = len / sliceSizeKeys / nThreads;
	uint64 startSlice = t->threadID * slicesPerThread;

	Job jb = ComputeSliceStart(src, startSlice);
	Job endjb = { 256, 0, 0 };
	if (t->threadID < nThreads - 1)
		endjb = ComputeSliceStart(src, (t->threadID + 1) * slicesPerThread);			// where the next thread begins


	//printf("-------------- L5b --------------------\n");

	// compute nextPfn single-threaded
	uint64 pagesPerSlice = sliceSizeBytes / pageSizeBytes;
	uint64 pos = startSlice * pagesPerSlice, obtained = 0;
	uint64 mask = sliceSizeBytes - 1;
	uint64 avoidmemcpy = (sliceSizeBytes == pageSizeBytes);
	int initialThread = jb.thread;
	uint64 slice = jb.slice;
	bool keepGoing = true;

	// CALCULATE THE BIT-SHIFT POWER DIRECTLY FROM PAGE SIZE HERE:
	unsigned long pageSizeBytePower = 12; // Default for 4KB (4096) pages
	if (pageSizeBytes == 2097152) {
		pageSizeBytePower = 21; // For 2MB Large Pages
	}
	else if (pageSizeBytes != 4096) {
		_BitScanForward64(&pageSizeBytePower, pageSizeBytes);
	}

	char* inputLocal = (char*)input;
	uint64* nextPfnLocal = nextPfn, * pfnLocal = pfn;
	// Use our new local variable here:
	uint64 pageSizeKeyPowerLocal = pageSizeBytePower;

	for (int i = jb.buck; i < 256 && keepGoing; i++)
	{
		for (int th = initialThread; th < nThreads && keepGoing; th++)
		{
			void** sliceBegin = ts[th].sliceDatabaseStart[src][i] + slice, ** sliceEnd = ts[th].sliceDatabase[src][i];
			uint64 slicesInBucket = sliceEnd - sliceBegin;

			if (i == endjb.buck && th == endjb.thread)
			{
				keepGoing = false;
				slicesInBucket = endjb.slice - slice;
			}

			for (int j = 0; j < slicesInBucket; j++)
			{
				// only count pointers that begin a slice; only the first slice can be partial
				char* p = (char*)sliceBegin[j];
				if (((uint64)p & mask) == 0)
				{
					// Fixed: now shifts by the local byte power variable
					uint64 pfnIdx = (p - inputLocal) >> pageSizeBytePower;
					if (avoidmemcpy)
						nextPfnLocal[pos++] = pfnLocal[pfnIdx];
					else
					{
						memcpy(nextPfnLocal + pos, pfnLocal + pfnIdx, pagesPerSlice * sizeof(uint64));
						pos += pagesPerSlice;
					}
					obtained++;
				}
			}
			slice = 0;
		}
		initialThread = 0;
	}

	// move the PFNs of the global stack
	uint64 globalStackSize = globalStackEnd - globalStackStart;
	uint64 chunk = globalStackSize / nThreads;
	uint64 startIdx = chunk * t->threadID;
	uint64 endIdx = t->threadID < nThreads - 1 ? startIdx + chunk : globalStackSize;
	pos = nPagesData + t->threadID * chunk * pagesPerSlice;

	for (uint64 j = startIdx; j < endIdx; j++)
	{
		// Fixed: shifts by the local byte power variable
		uint64 pfnIdx = ((char*)globalStackStart[j] - (char*)input) >> pageSizeBytePower;
		if (avoidmemcpy)
			nextPfn[pos++] = pfn[pfnIdx];
		else
		{
			memcpy(nextPfn + pos, pfn + pfnIdx, pagesPerSlice * sizeof(uint64));
			pos += pagesPerSlice;
		}
		obtained++;
	}

	// append local free slices at the end
	pos = nPagesData + globalStackSize * pagesPerSlice;
	for (int j = 0; j < t->threadID; j++)
	{
		ThreadState* thState = ts + j;
		pos += (thState->freeStackEnd - thState->freeStackStart) * pagesPerSlice;
	}

	for (void** p = t->freeStackStart; p < t->freeStackEnd; p++)
	{
		// Fixed: shifts by the local byte power variable
		uint64 pfnIdx = ((char*)*p - (char*)input) >> pageSizeBytePower;
		if (avoidmemcpy)
			nextPfn[pos++] = pfn[pfnIdx];
		else
		{
			memcpy(nextPfn + pos, pfn + pfnIdx, pagesPerSlice * sizeof(uint64));
			pos += pagesPerSlice;
		}
		obtained++;
	}

	// --------------------------- L5b
	br.WaitForAll(nThreads, true);

	if (MapUserPhysicalPages((char*)input + off * pageSizeBytes, unmapLoad, nextPfn + off) == FALSE)
	{
		printf("[%d] Map-reshuffle failed with %d\n", t->threadID, GetLastError());
		exit(-1);
	}
}

void Typhoon::ReturnExtraSlices(ThreadState* t, uint64 threshold)
{
	// return anything above the threshold to the global stack
	uint64 mySize = t->freeStackEnd - t->freeStackStart;
	if (mySize > threshold)
	{
		uint64 extra = mySize - threshold;
		void*** ptr = (void***)InterlockedAdd64((volatile LONG64*)&globalStackEnd, extra * sizeof(void*));
		t->freeStackEnd -= extra;
		memcpy(ptr - extra, t->freeStackEnd, extra * sizeof(void*));
	}
}

void Typhoon::RefillSlices(ThreadState* t, uint64 threshold)
{
	// refill stacks back to threshold
	uint64 mySize = t->freeStackEnd - t->freeStackStart;
	if (mySize < threshold)
	{
		uint64 extra = threshold - mySize;
		uint64 stackSize = globalStackEnd - globalStackStart;
		if (stackSize < extra)
		{
			printf("popping %lld slices from global stack with size %lld\n", extra, stackSize);
			exit(-1);
		}
		// use InterlockedAdd to implement InterlockedSubtract; this must be in bytes
		uint64 extraNegative = (mySize - threshold) * sizeof(void*);
		void*** ptr = (void***)InterlockedAdd64((volatile LONG64*)&globalStackEnd, extraNegative);
		memcpy(t->freeStackEnd, ptr, extra * sizeof(void*));
		t->freeStackEnd += extra;
	}
}

void Typhoon::SetupDatabaseFromDump(ThreadState* t, int src, DumpRecord* end)
{
	uint64 c[256];
	memset(c, 0, sizeof(*c) * 256);

	// count 
	for (DumpRecord* p = t->sliceDump; p < end; p++)
	{
		if (p->idx > 255) ReportError("index out of bounds\n");
		c[p->idx]++;
	}

	// set up the start of each bucket
	void*** sdLocalStart = t->sliceDatabaseStart[src];
	void*** sdLocal = t->sliceDatabase[src];
	void** sdBuffer = t->sdBuffer[src];
	for (int i = 0; i < 256; i++)
	{
		if (i > 0)
			sdLocalStart[i] = sdLocalStart[i - 1] + c[i - 1] + 1;		// add one dummy slice for size
		else
			sdLocalStart[0] = sdBuffer;
	}

	memcpy(sdLocal, sdLocalStart, sizeof(*sdLocal) * 256);

	// move the slice pointers from the dump to the database
	for (DumpRecord* p = t->sliceDump; p < end; p++)
	{
		void** ptr = sdLocal[p->idx];
		*ptr = p->ptr;
		sdLocal[p->idx] = ptr + 1;
	}
}

void Typhoon::SetupDatabaseFromHistogram(ThreadState* t, int src)
{
	// use t->cnt to decide the size of each bucket in sd
	void*** sdLocalStart = t->sliceDatabaseStart[src];
	void** sdBuffer = t->sdBuffer[src];

	for (int i = 0; i < 256; i++)
	{
		if (i > 0)
			// round cnt down to slice size, then add first/last partial slices 
			sdLocalStart[i] = sdLocalStart[i - 1] + (t->cnt[i - 1] >> sliceSizeKeyPower) + 2;
		else
			sdLocalStart[0] = sdBuffer;
	}

	memcpy(t->sliceDatabase[src], t->sliceDatabaseStart[src], sizeof(*t->sliceDatabaseStart[src]) * 256);
}