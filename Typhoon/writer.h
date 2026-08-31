/**
 * @file  writer.h
 * @brief example data generator
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
#include <random>
#include <stdexcept>
#include <thread>
#include <stack>
#include <queue>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <cassert>
using namespace std;

#pragma warning( push )
#pragma warning( disable : 4244 )

#define MAX_PRINTOUT	1024
#define PRINT(fmt, ...) { char buf_PRINT[MAX_PRINTOUT] = "%s: "; strcat_s(buf_PRINT, MAX_PRINTOUT, fmt); printf (buf_PRINT, __FUNCTION__, ##__VA_ARGS__); }
#define ReportError(fmt, ...) { PRINT(fmt, ##__VA_ARGS__); \
								exit(-1); }

typedef unsigned __int64 uint64;
typedef __int64 int64;

using namespace std;
using namespace std::chrono;
using hrc = high_resolution_clock;

// typedef Item
typedef int64_t  i64;
typedef uint64_t ui64;
typedef uint32_t ui;
typedef uint8_t	uchar;
typedef uint16_t ushort;

// typedef simd stuff
typedef __m128i sse;
typedef __m128	ssef;
typedef __m128d	ssed;
typedef __m256i avx2;
typedef __m256	avx2f;
typedef __m256d avx2d;
//typedef __m512i avx512;
//typedef __m512	avx512f;
//typedef __m512d avx512d;

#pragma pack(push, 1)
template <typename Keytype, typename Valuetype>
struct KeyValue {
	Keytype key;
	Valuetype value;
	bool operator <(const KeyValue& kv) const {
		return key < kv.key;
	}
	bool operator >(const KeyValue& kv) const {
		return key > kv.key;
	}
	bool operator !=(const KeyValue& kv) const {
		return key != kv.key;
	}
	bool operator <=(const KeyValue& kv) const {
		return key <= kv.key;
	}
};
#pragma pack(pop)
template struct KeyValue<ui, ui>;
template struct KeyValue<ui64, ui64>;

#define MIN(x, y)				((x)<(y)?(x):(y))
#define MAX(x, y)				((x)<(y)?(y):(x)) 
#define FOR(i,n,k)				for (ui64 (i) = 0; (i) < (n); (i)+=(k)) 
#define FOR_INIT(i, init, n, k)	for (ui64 (i) = (init); (i) < (n); (i) += (k)) 
#define PRINT_ARR(arr, n)		{ FOR((i), (n), 1) printf("%lX ", (arr)[(i)]); printf("\n"); }
#define PRINT_ARR64(arr, n)		{ FOR((i), (n), 1) printf("%llX ", ((ui64*)arr)[(i)]); printf("\n"); }
#define PRINT_DASH(n)			{ FOR(i, (n), 1) printf("-"); printf("\n"); }
#define ELAPSED(st, en)			( duration_cast<duration<double>>(en - st).count() )
#define ELAPSED_MS(st, en)		( duration_cast<duration<double, std::milli>>(en - st).count() )
#define ELAPSED_NS(st, en)		( duration_cast<duration<double, std::nano>>(en - st).count() )
#define NOINLINE				__declspec(noinline)
#define KB(x)					(x << 10)
#define MB(x)					(x << 20)
#define GB(x)					(x << 30)
#define HERE(x)					printf("Here %3lu\n", (x));
#define MAX_PATH_LEN			512
#define MAX_PRINTOUT			1024
#define PRINT(fmt, ...)			{ char buf_PRINT[MAX_PRINTOUT] = "%s: "; strcat_s(buf_PRINT, MAX_PRINTOUT, fmt); printf (buf_PRINT, __FUNCTION__, ##__VA_ARGS__); }
#define ROUND_UP(x,R)			((  ((x) + (R)-1) / (R) ) * (R) )
#define ROUND_DOWN(x, s)		((x) & ~((s)-1))
#define NINE_BIT_MASK			((1<<9) - 1)
#define LOAD(rg, ptr)			{ rg = *(ptr); }
#define STORE(rg, ptr)			{ *(ptr) = rg; }
#define VALLOC(sz)				(VirtualAlloc(NULL, (sz), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
#define VFREE(ptr)				(VirtualFree((ptr), 0, MEM_RELEASE))

#include <unordered_set>
#include <fstream>
#include <execution>

//#define PLD_L2
//#define PARETO_L2


namespace datagen {

	typedef enum {
		PARETO_SHUFF,
		ALMOST_SORTED,
		NORMAL,
		UNIFORM_DBL,
	} WRITER_TYPE;

#define UNIFORM_64			0
#define UNIFORM_32			1
#define UNIFORM_N_BY_4		2
#define UNIFORM_N			3
#define UNIFORM_3N			4
#define UNIFORM_10N			5
#define UNIFORM_UPPER_N		6
#define UNIFORM_CUSTOM		7

	//#define MIN(x,y)	((x) < (y) ? (x) : (y))

	static const char* uniform_ranges[] = { "2^64 - 1", "2^32 - 1", "n/4", "n-1", "3n-1", "10n-1", "[MAX - n + 1, MAX]", "Custom [l, m]" };
	static const int uniform_ranges_count = 8;

	static const char* writer_names_[] = { "Pareto-shuffled",
									"Almost-sorted", "Normal", "Uniform-double" };

	static const char* PLD_path = "PLD-out-graph-1GB.adj";		// path on d5		"A:\\PLD-out-graph.adj
	const int writer_count_ = 22;

	template <typename Item, typename Key = Item>
	class Writer {
	public:
		void pareto_writer(Item* A, ui64 n, WRITER_TYPE type) {
			ui64 a = 6364136223846793005, c = 1442695040888963407, x = 1;
			double ED = 20;
			double alpha = 1, beta = 7;
			ui64 sum = 0, Items = 0, y = 889;
			ui64 maxF = 0;
			for (ui64 i = 0; i < n; ) {
				x = x * a + c;
				y = y * a + c;

				// generate frequency from the Pareto distribution with alpha=1; otherwise, the generator gets slow
				double u = (double)y / ((double)(1LLU << 63) * 2);			// uniform [0,1]
				ui64 f = MIN(ceil(beta * (1 / (1 - u) - 1)), 10000);		// rounded-up Pareto
				if (i + f < n) {
					FOR(j, f, 1) {
						if constexpr (std::is_same<Item, ui64>::value && std::is_same<Key, ui>::value) {
							ui64 kv = x;
							kv = (kv << 32) | i;
							A[i + j] = kv;
						}
						else if constexpr (std::is_same<Item, KeyValue<ui64, ui64>>::value) {
							KeyValue<ui64, ui64> kv;
							kv.key = x;
							kv.value = i;
							A[i + j] = kv;
						}
						else A[i + j] = x;
					}
					i += f;
				}
				else if (i + 10 >= n) {
					for (; i < n; ++i) {
						if constexpr (std::is_same<Item, ui64>::value && std::is_same<Key, ui>::value) {
							ui64 kv = x;
							kv = (kv << 32) | i;
							A[i] = kv;
						}
						else if constexpr (std::is_same<Item, KeyValue<ui64, ui64>>::value) {
							KeyValue<ui64, ui64> kv;
							kv.key = x;
							kv.value = i;
							A[i] = kv;
						}
						else A[i] = x;
					}
				}
			}
			std::random_device rd;
			std::mt19937_64 g(rd());
			std::shuffle(A, A + n, g);
		}

		// set every 7 key of a sorted sequence to MAX
		void almost_sorted(Item* A, ui64 n) {
			ui64 m = ~0ull;
			ui64 l = 0;
			if constexpr (std::is_same<Item, Key>::value) {
				if constexpr (std::is_same<Item, ui>::value) {
					std::mt19937 g;
					std::uniform_int_distribution<Item> d(l, MIN(m, UINT32_MAX));
					FOR(i, n, 1) A[i] = d(g);
				}
				else if constexpr (std::is_same<Item, ui64>::value) {
					std::mt19937_64 g;
					std::uniform_int_distribution<Item> d(l, m);
					FOR(i, n, 1) A[i] = d(g);
				}
				else
					ReportError("Type not supported");
			}
			// else -- key-value pair
			else {
				if constexpr (std::is_same<Item, ui64>::value && std::is_same<Key, ui>::value) {
					std::mt19937 g;
					std::uniform_int_distribution<ui> d(l, MIN(m, UINT32_MAX));
					FOR(i, n, 1) {
						ui64 kv = d(g);
						kv = (kv << 32) | i;		// add value to LSB
						A[i] = kv;
					}
				}
				else if constexpr (std::is_same<Item, KeyValue<ui64, ui64>>::value) {
					std::mt19937_64 g;
					std::uniform_int_distribution<ui64> d(l, m);
					FOR(i, n, 1) {
						KeyValue<ui64, ui64> kv;
						kv.key = d(g);
						kv.value = i;
						A[i] = kv;
					}
				}
				else
					ReportError("Type not supported");
			}
			std::sort(std::execution::par_unseq, A, A + n);
			FOR(i, n, 7) {
				if constexpr (std::is_same<Item, ui64>::value && std::is_same<Key, ui>::value) A[i] = (UINT32_MAX << 32) | i;
				else if constexpr (std::is_same<Item, ui64>::value) A[i] = UINT64_MAX;
				else if constexpr (std::is_same<Item, ui>::value) A[i] = UINT32_MAX;
				else if constexpr (std::is_same<Item, KeyValue<ui64, ui64>>::value) A[i].key = UINT64_MAX;
			}
		}

		// normal distribution w/ miu = max/2 and sigma = max/6
		void normal(Item* A, ui64 n, ui64 miu = (UINT64_MAX >> 1), ui64 sigma = (UINT64_MAX >> 1) / 3) {
			//printf("> Mean: %llu, std. dev: %llu\n", miu, sigma);
			if constexpr (std::is_same<Item, ui>::value) {
				miu = UINT32_MAX >> 1;
				sigma = (UINT32_MAX >> 1) / 3;
				std::mt19937 gen;
				std::normal_distribution<> dis(miu, sigma);

				FOR(i, n, 1)
					A[i] = std::round(dis(gen));
			}
			else if constexpr (std::is_same<Item, ui64>::value && std::is_same<Key, ui>::value) {
				miu = UINT32_MAX >> 1;
				sigma = (UINT32_MAX >> 1) / 3;
				std::mt19937 gen;
				std::normal_distribution<> dis(miu, sigma);

				FOR(i, n, 1) {
					ui64 key = std::round(dis(gen));
					A[i] = (key << 32) | i;
				}
			}
			else if constexpr (std::is_same<Item, ui64>::value) {
				miu = UINT64_MAX >> 1;
				sigma = (UINT64_MAX >> 1) / 3;
				std::mt19937_64 gen;
				std::normal_distribution<> dis(miu, sigma);

				FOR(i, n, 1)
					A[i] = std::round(dis(gen));
			}

			else if constexpr (std::is_same<Item, KeyValue<ui64, ui64>>::value) {
				miu = UINT64_MAX >> 1;
				sigma = (UINT64_MAX >> 1) / 3;
				std::mt19937_64 gen;
				std::normal_distribution<> dis(miu, sigma);

				FOR(i, n, 1) {
					KeyValue<ui64, ui64> kv;
					kv.key = std::round(dis(gen));; kv.value = i; A[i] = kv;
				}
			}
			else
				ReportError("Type not supported");
		}

		void uniform_double(Item* A, ui64 n) {
			if constexpr (std::is_same<Item, ui64>::value && std::is_same<Key, ui>::value) {
				std::uniform_real_distribution<float> dis(0.0, FLT_MAX);
				std::mt19937 gen;
				double* p = (double*)A;
				FOR(i, n, 1)
					p[i] = dis(gen);
				FOR(i, n, 1) A[i] = (A[i] & 0xFFFFFFFF00000000) | i;
			}
			else if constexpr (std::is_same<Item, ui>::value) {
				float* p = (float*)A;
				std::uniform_real_distribution<float> dis(0.0, FLT_MAX);
				std::mt19937 gen;
				FOR(i, n, 1)
					p[i] = dis(gen);
			}

			else if constexpr (std::is_same<Item, ui64>::value) {
				double* p = (double*)A;
				std::uniform_real_distribution<double> dis(0.0, DBL_MAX);
				std::mt19937_64 gen;
				FOR(i, n, 1)
					p[i] = dis(gen);
			}
			else if constexpr (std::is_same<Item, KeyValue<ui64, ui64>>::value) {
				std::uniform_real_distribution<double> dis(0.0, DBL_MAX);
				std::mt19937_64 gen;
				FOR(i, n, 1) {
					KeyValue<ui64, ui64> kv;
					kv.key = dis(gen); kv.value = i; A[i] = kv;
				}
			}

		}

		// l, m is the range for MT random Items only
		void generate(Item* A, ui64& n, WRITER_TYPE type, ui64 m = ~0ull, ui64 l = 0, ui64 rep = 1, ui64 X1 = 1, ui64 X2 = 1) {
			//printf("> Writer: %s ... ", writer_names_[type]);
			if (type == ALMOST_SORTED)
				almost_sorted(A, n);
			else if (type == NORMAL)
				normal(A, n);
			else if (type == UNIFORM_DBL)
				uniform_double(A, n);
			if (type == PARETO_SHUFF)
				pareto_writer(A, n, type);
		}
	};
};

#pragma warning( pop )
