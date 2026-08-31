/**
 * @file  main.cpp
 * @brief example benchmark of Typhoon slice-scrambled LSD radix sort algorithm
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
#include "Typhoon.h"
#include <algorithm>
#include "writer.h"

__declspec(noinline)
void Create(void* list, uint64_t len, int seed, int keyType)
{
	sfmt_t s;
	sfmt_init_gen_rand(&s, seed);

	if (keyType == 32)
	{
		uint32_t* list32 = static_cast<uint32_t*>(list);
		for (uint64_t i = 0; i < len; i++)
		{
			list32[i] = (uint32_t)sfmt_genrand_uint32(&s);
		}
	}
	else
	{
		uint64_t* list64 = static_cast<uint64_t*>(list);
		// constructing KV pairs: uint64 item = (key << 32) + value
		for (uint64_t i = 0; i < len; i++)
		{
			list64[i] = (uint64_t)sfmt_genrand_uint64(&s);
		}
	}
}

bool Test(void* list, uint64_t len, int keyType)
{
	if (len == 0) return true;
	uint64_t count = 0;

	if (keyType == 32)
	{
		uint32_t* list32 = static_cast<uint32_t*>(list);
		uint32_t prev = list32[0];
		for (uint64_t i = 1; i < len; i++)
		{
			uint32_t cur = list32[i];
			if (cur < prev)
			{
				printf("[%4lld] violation @ %5lld ", count++, i);
				printf("    prev %X, cur %X \n", prev, cur);
				return false;
			}
			prev = cur;
		}
	}
	else
	{
		uint64_t* list64 = static_cast<uint64_t*>(list);
		// 64-bit path: extract upper 32-bit key to verify sortedness
		uint64_t prev = list64[0] >> 32;
		for (uint64_t i = 1; i < len; i++)
		{
			uint64_t cur = list64[i] >> 32;
			if (cur < prev)
			{
				printf("[%4lld] violation @ %5lld ", count++, i);
				printf("    prev %llX, cur %llX \n", prev, cur);
				return false;
			}
			prev = cur;
		}
	}
	return true;
}

//#define DISTRIBUTION_BENCHMARKS

int main(int argc, char** argv)
{
	datagen::WRITER_TYPE writer_types[] =
	{
		datagen::WRITER_TYPE::ALMOST_SORTED,
		datagen::WRITER_TYPE::PARETO_SHUFF,
		datagen::WRITER_TYPE::NORMAL,
		datagen::WRITER_TYPE::UNIFORM_DBL,
	};

	int nThreads = (argc >= 2) ? atoi(argv[1]) : 1;
	int GB = (argc >= 3) ? atoi(argv[2]) : 1;
	int keyType = (argc >= 4) ? atoi(argv[3]) : 32; // full sort on 32-bit keys, (key,value) pairs with the higher half being the key

	size_t keySizeByte = (keyType == 32) ? sizeof(uint32_t) : sizeof(uint64_t);
	uint64_t len = ((uint64)GB << 30) / keySizeByte;
	LARGE_INTEGER freq, start, end;
	QueryPerformanceFrequency(&freq);

	uint64_t memory = len * keySizeByte;
	Typhoon mlsd(len, nThreads, keyType);
	void* input = mlsd.GetInputBuffer();

	printf("----------------------------------------------------\n");
	printf("%lld MB of %d-bit items with %d thread(s) (WC_LINE = %d, SLICE = %lld KB)\n",
		((memory + (1 << 19)) >> 20), keyType, nThreads, mlsd.wcLineBits, mlsd.sliceSizeBytes / (1 << 10));
	printf("----------------------------------------------------\n");

	int rep = 3;
#ifdef DISTRIBUTION_BENCHMARKS		
	for (auto type : writer_types)
	{
		printf(" %-20s\n", datagen::writer_names_[type]);
#endif
		for (int i = 0; i < rep; i++)
		{
			printf("*");
#ifdef DISTRIBUTION_BENCHMARKS
			if (keyType == 32) {
				datagen::Writer<uint32_t> writer;
				writer.generate(static_cast<uint32_t*>(input), len, (datagen::WRITER_TYPE)type);
			}
			else {
				datagen::Writer<uint64_t, uint32_t> writer;
				writer.generate(static_cast<uint64_t*>(input), len, (datagen::WRITER_TYPE)type);
			}
#else
			Create(input, len, 55, keyType);
#endif
			printf("* ");

			QueryPerformanceCounter(&start);
			mlsd.RunAllThreads(5);
			QueryPerformanceCounter(&end);

			double duration = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;
			printf("    %5.3f sec %4.0f M/sec [", duration, len / duration / 1e6);
			for (int j = 0; j <= 5; j++)
				printf("%5.0f ", len / ((double)mlsd.elapsed[j] / freq.QuadPart) / 1e6);
			printf("\b] ");

			if (Test(input, len, keyType))
				printf("--> passed");
			else
				printf("--> failed");
			putchar('\n');
		}
#ifdef DISTRIBUTION_BENCHMARKS		
	}
#endif
}