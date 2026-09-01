# Typhoon: A Slice-Scrambled In-Place LSD Sort

This repository contains the implementation of the algorithm described in the [**Typhoon paper**](https://ieeexplore.ieee.org/document/11402641). The research was developed by [**Texas A&M University Internet Research Lab**](http://irl.cs.tamu.edu/projects/streams/), and published in [**IEEE BigData 2025**](https://bigdataieee.org/). If you have trouble viewing the paper, here is a [**copy**](https://irl.cse.tamu.edu/people/zelun/papers/bigdata2025.pdf) on my research website. You can also view the virtual [**presentation**](https://www.computer.org/csdl/video-library/video/2dOz2ocQYgg) and download the associated [**slides**](https://irl.cse.tamu.edu/people/zelun/papers/bigdata2025-ppt.pdf).

## Performance

A quick preview of Typhoon's performance is given in the tables below. For deeper analysis, please refer to the paper. Benchmarks of Typhoon is compiled in Visual Studio 2019, while prior methods are reported using the best achievable speed among Clang 19, Intel oneAPI C++ 2025 (ICX), and VS 2019.

The following table shows the performance comparison of Typhoon with prior works across a range of CPU architectures. It compares the sort speed of 32-bit uniformly random integer keys with input sizes close to the memory limit generated using the SFMT (SIMD-oriented Fast Mersenne Twister) library included in the codebase in single core. Typhoon is 38 times faster than std::sort on the AMD Zen 5 architecture. The final row shows Typhoon's speedup against the fastest competitor. Dashes indicate inability to finish the sort. 

<div align="center">

Single-Core Speed (M key/sec) on 32-bit keys

| Algorithm | SB | IB | BW | CL | AL | Zen4 | Zen5 |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| [Gorset](https://github.com/gorset/radix) | 25 | 26 | 24 | 48 | 46 | 71 | 73 |
| [Polychroniou](https://dl.acm.org/doi/10.1145/2588555.2610522) | 20 | 21 | 23 | 35 | 44 | 49 | 53 |
| [Ska](https://probablydance.com/2016/12/27/i-wrote-a-faster-sorting-algorithm/) | 40 | 43 | 41 | 84 | 99 | 116 | 120 |
| [Regions](https://dl.acm.org/doi/abs/10.1145/3323165.3323198) | 53 | *57* | 52 | 89 | 124 | 121 | 142 |
| [Vortex](https://dl.acm.org/doi/10.1145/3373376.3378527) | *54* | 56 | 53 | *162* | *178* | *203* | *265* |
| [IPS2Ra](https://dl.acm.org/doi/full/10.1145/3505286) | – | – | 47 | 90 | 109 | 110 | 121 |
| [pdqsort](https://github.com/orlp/pdqsort) | 23 | 24 | 24 | 33 | 40 | 45 | 47 |
| [IPS4o](https://dl.acm.org/doi/full/10.1145/3505286) | – | – | 22 | 33 | 34 | 46 | 50 |
| [Highway](https://arxiv.org/abs/2205.05982) | 21 | 22 | 42 | 77 | 106 | 149 | 185 |
| [Intel](https://github.com/numpy/x86-simd-sort) | – | – | *63* | 118 | 167 | 177 | 240 |
| std::sort | 7 | 7 | 7 | 9 | 10 | 13 | 13 |
| **Typhoon** | **120** | **129** | **129** | **265** | **328** | **388** | **491** |
| *Speedup vs Next-Best* | *2.2×* | *2.3×* | *2.0×* | *1.6×* | *1.8×* | *1.9×* | *1.8×* |

</div>

> **Note:** CPU Architectures: Sandy Bridge (SB), Ivy Bridge (IB), Broadwell (BW), Coffee Lake (CL), Alder Lake (AL), Raphael (Zen 4), and Granite Ridge (Zen 5). **Bold** and *italics* indicate the best and the next-best performing algorithm respectively. 

Typhoon also achieves high-performance and near perfect scaling in multi-core environment. The next table compares the sort speed of 8 GB 64-bit key-value pairs of various data distributions available either in the codebase or online, on an 8-core Intel Skylake-X (i7-7820X) CPU using all cores. If you are interested in the scaling experiments, please refer to Table VIII from the paper.

<div align="center">

All-Core Speed (M key/sec) on 64-bit key-value pairs
  
| Algorithm | D1 | D2 | D3 | D4 | D5 | G |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| [Raduls2](https://arxiv.org/abs/1703.00687) | *656* | 478 | 394 | *737* | 433 | *491* |
| [Regions](https://dl.acm.org/doi/abs/10.1145/3323165.3323198) | 365 | 382 | 359 | 351 | 296 | 291 |
| [Voracious](https://github.com/lakwet/voracious_sort) | 402 | 510 | 397 | 405 | 340 | 298 |
| [IPS2Ra](https://dl.acm.org/doi/full/10.1145/3505286) | 409 | *737* | *623* | 418 | *466* | 318 |
| [Dovetail](https://dl.acm.org/doi/10.1145/3627535.3638483) | 198 | 206 | 177 | 197 | 197 | 130 |
| [IPS4o](https://dl.acm.org/doi/full/10.1145/3505286) | 286 | 366 | 351 | 291 | 341 | 286 |
| [Origami](https://dl.acm.org/doi/abs/10.14778/3489496.3489507) | 380 | 389 | 395 | 394 | 395 | 392 |
| **Typhoon** | **986** | **989** | **998** | **986** | **1,001** | **997** |
| *Speedup vs Next-Best* | *1.5×* | *1.3×* | *1.6×* | *1.3×* | *2.1×* | *2.0×* |

</div>

> **Distributions:** 
> * **D1:** Uniformly random integers as in SFMT.
> * **D2:** An almost-sorted sequence of uniform integers, where every 7th key is set to `UINT_MAX`.
> * **D3:** Uniformly random integer keys each repeated *U* times (where *U* is drawn from a Zipf distribution with α = 1, β = 7), and then shuffled randomly.
> * **D4:** Integer keys drawn from a normal distribution with a mean of `UINT32_MAX / 2` and a standard deviation equal to 1/3 of the mean.
> * **D5:** Uniformly random floats between 0 and `FLT_MAX`.
> * **G:** an inter-domain out-graph from the IRLbot web crawl, consisting of 89M nodes and 1.8B edges for in-degree computation or graph inversion tasks. 
> 
> **D1** is turned on by default, **D2–D5** are generated via `writer.h` and can be turned on if you uncomment the `DISTRIBUTION_BENCHMARKS` macro in `main.cpp`. The graph dataset is available in the Datasets Section [here](https://irl.cse.tamu.edu/projects/web/). *(Note: The raw graph requires pre-processing to flatten the adjacency lists into standard 32-bit keys or 64-bit key-value pairs).*

## Getting Started

### Recommended Setup

This is my setup for the experiments (other versions should also work):
- **OS:** Windows Server 2016
- **Compiler:** MSVC++ 14.29 (Visual Studio 2019 16.11)
- **Permission:** ```SeLockMemoryPrivilege``` needs to be enabled in order to allocate, map and unmap physical pages in user space required by Typhoon. To grant this permission:
  1. Click the Windows icon, type `secpol.msc` to open the Local Security Policy editor.
  2. Navigate to **Security Settings > Local Policies > User Rights Assignment**.
  3. Double-click **Lock pages in memory** and add either your Windows user account or Administrators if you are an admin.
  4. Sign out and log back in (or restart the machine) for the token changes to take effect.

### Build

1. Ensure the project configuration is set to **x64 Release**.
2. Set the C++ standard to **C++17** under `Project > Properties > Configuration Properties > General`.
3. Set the appropriate compiler flags (e.g., `/arch:AVX2`) under `Project > Properties > Configuration Properties > C/C++ > Command Line`.

*(Note: Steps 2 and 3 are already pre-configured in the provided Visual Studio solution. However, if you are integrating Typhoon's source code into a custom project, you may need to apply these settings again manually.)*
   
### Usage

Instantiating `Typhoon` initializes the required metadata bookkeeping as well as the input array allocated in physical pages. The constructor takes in the length of data as in the number of keys, the number of threads each pinned to individual core that you'd like to use, and the key type. 

*The current implementation supports 32-bit keys and 64-bit key-value pairs with the upper half being the key.*

```cpp
// Initialize Typhoon(len, nThreads, keyType), example with 1GB 32-bit key
Typhoon mlsd((1<<28), 1, 32);

// Retrieve the allocated buffer to populate your data
void* input = mlsd.GetInputBuffer();
```

To run the sort, simply call the execution method:

```cpp
// Execute the sort 
// (Pass '5' because there are 5 internal levels in Typhoon)
mlsd.RunAllThreads(5);
```

### Parameter Autotuning

Typhoon requires three parameters to be tuned for maximum hardware-specific performance:
1. `WC_LINE`: Size of the in-cache software write-combine buffer before stream write to memory.
(the major parameter)
2. `SIMD`: Whether to collect the final histogram counts using SIMD or scalar methods.
3. `PrefetchT2`: Whether to collect the final histogram counts with prefetches to the L3 cache, or to all cache hierarchy.

For ease of use, Typhoon will automatically tune these parameters for you the first time it is run. It can handle parameters for different machines and different key sizes. The optimal results are then saved locally to a `config.ini` file using the following format:

```ini
# Typhoon Profiler Configuration File

[Intel(R) Core(TM) i7-7820X CPU @ 3.60GHz]
    family=6
    model=55
    stepping=4
    {32-bit}
        WC_LINE=6
        SIMD=1
        PrefetchT2=1
    {64-bit}
        WC_LINE=5
        SIMD=1
        PrefetchT2=1
```

## Citation

If you use Typhoon for your research or project, please consider citing our paper:

Z. Liu, A. Arman and D. Loguinov, "Typhoon: A Slice-Scrambled In-Place LSD Sort," *2025 IEEE International Conference on Big Data (BigData)*, 2025, pp. 191-200.

```bibtex
@INPROCEEDINGS{liu2025typhoon,
  author={Liu, Zelun and Arman, Arif and Loguinov, Dmitri},
  booktitle={2025 IEEE International Conference on Big Data (BigData)}, 
  title={Typhoon: A Slice-Scrambled In-Place LSD Sort}, 
  year={2025},
  pages={191-200},
  doi={10.1109/BigData66926.2025.11402641}
}
```



## License

This project is licensed under the BSD-3-Clause License - see the [**LICENSE**](LICENSE.txt) file for details.

## Authors

[**Zelun Liu**](https://irl.cse.tamu.edu/people/zelun/), [**Arif Arman**](https://arif-arman.github.io/), [**Dmitri Loguinov**](https://irl.cs.tamu.edu/people/dmitri/)
