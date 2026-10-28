# Standard library performance

How the Kpp containers (stdlib/core) compare with the three C++ standard libraries, what was
changed to close the gaps, and what is left, split into library design and compiler codegen.

## Method

`compiler\bench\stdlib\run.cmd [DIR]` builds and runs `compiler/bench/stdlib/bench.kpp` and its
line-for-line C++ twin `bench.cpp` (25 micro-benchmarks, each returning a checksum that must match
across all columns) and prints the table below. Every benchmark runs in a process of its own and
reports the best of 7 rounds: the Windows heap's speed depends on what earlier code allocated (a
26-byte malloc/free pair measured 18 ns alone and 180 ns after other benchmarks in the same
process), so running them in one process made results depend on their order.

Columns:
- **Kpp, before**: the stdlib at 117ba0a, built with `--std` (DIR argument of run.cmd).
- **Kpp**: `kppc --release`, the default split into IR modules (two files per thread).
- **Kpp, 1 module**: `kppc --release --modules 1`: the same program as one LLVM module, so clang
  can inline across files. The difference to the previous column is a codegen gap, not a
  library one.
- **MSVC STL**: clang 19.1.5 `-O2 -std=c++20`, Windows, same machine.
- **libstdc++** (g++ 13.3 `-O2`) and **libc++** (clang 18.1 `-O2 -stdlib=libc++`): Ubuntu 24.04
  under WSL 2 on the same machine. Linux has another allocator (glibc malloc, which is much
  faster than the Windows heap for small blocks) and another page-fault path (the large-block
  benchmarks, like vector.copy int, pay for fresh pages there), so compare these two columns
  for the algorithms, and Kpp against MSVC STL for absolute numbers.

Current state (after the work below and the conformance changes: containers move elements with
move constructors, Map is a red-black tree, HashMap keeps entries in nodes, docs/stdlib/), PC-PRACOVNA
(Ryzen 9 9950X), 2026-10-28, milliseconds, lower is better. The fasthash rows run FastHashMap in
Kpp and std::unordered_map again in C++:

| benchmark | Kpp | Kpp, 1 module | MSVC STL | libstdc++ | libc++ |
|---|---:|---:|---:|---:|---:|
| vector.push int | 14.4 | 15.4 | 19.2 | 22.2 | 21.5 |
| vector.push reserved | 5.2 | 5.8 | 8.7 | 11.4 | 10.8 |
| vector.resize | 2.7 | 2.6 | 2.6 | 9.3 | 10.4 |
| vector.iterate | 10.0 | 9.8 | 9.8 | 16.8 | 8.8 |
| vector.copy int | 42.1 | 41.5 | 41.8 | 114.6 | 112.8 |
| vector.insert front | 45.9 | 46.3 | 45.9 | 25.7 | 24.6 |
| vector.push String | 19.1 | 16.8 | 26.3 | 18.9 | 9.5 |
| string.small | 88.7 | 85.1 | 85.9 | 75.0 | 84.6 |
| string.vector copy | 22.4 | 18.2 | 29.5 | 10.0 | 4.2 |
| string.append char | 13.9 | 10.4 | 11.4 | 6.8 | 14.1 |
| string.append pieces | 13.4 | 12.8 | 13.9 | 10.1 | 8.3 |
| string.concat | 29.1 | 22.2 | 20.3 | 7.3 | 7.3 |
| string.find | 6.7 | 6.5 | 2.2 | 6.1 | 6.0 |
| string.find char | 2.6 | 4.4 | 1.2 | 0.9 | 0.9 |
| string.compare | 21.2 | 15.4 | 11.2 | 8.1 | 12.7 |
| sort.int | 45.1 | 43.6 | 47.9 | 39.8 | 38.8 |
| sort.int predicate | 43.0 | 43.1 | 48.0 | 39.0 | 38.8 |
| sort.String | 161.9 | 126.4 | 142.6 | 147.9 | 129.9 |
| hash.int insert | 67.5 | 67.8 | 116.6 | 82.5 | 91.7 |
| hash.int lookup | 132.4 | 124.3 | 256.8 | 86.1 | 103.4 |
| hash.String insert | 171.7 | 168.1 | 242.5 | 215.4 | 227.3 |
| hash.String lookup | 116.6 | 111.9 | 164.0 | 143.2 | 150.8 |
| fasthash.int insert | 27.5 | 24.6 | 111.7 | 80.7 | 95.3 |
| fasthash.int lookup | 74.7 | 67.4 | 252.3 | 91.3 | 106.7 |
| fasthash.String insert | 58.4 | 52.1 | 240.2 | 213.9 | 227.0 |
| fasthash.String lookup | 78.2 | 75.4 | 167.0 | 142.7 | 148.4 |
| map.insert | 14.2 | 14.1 | 12.7 | 11.0 | 10.0 |
| map.lookup | 70.7 | 72.9 | 69.0 | 94.0 | 59.9 |
| map.iterate | 17.4 | 17.2 | 14.9 | 7.9 | 5.7 |

The first pass, with the stdlib before this work in the first column (Map was then a sorted
Vector, HashMap a flat table without std's guarantees):

| benchmark | Kpp, before | Kpp | Kpp, 1 module | MSVC STL | libstdc++ | libc++ |
|---|---:|---:|---:|---:|---:|---:|
| vector.push int | 12.6 | 13.0 | 13.0 | 20.0 | 22.6 | 20.7 |
| vector.push reserved | 5.2 | 5.2 | 5.3 | 8.9 | 11.5 | 11.1 |
| vector.resize | 11.0 | 2.7 | 2.6 | 2.8 | 9.4 | 9.2 |
| vector.iterate | 10.0 | 9.9 | 9.9 | 9.9 | 16.9 | 8.9 |
| vector.copy int | 82.4 | 41.8 | 42.5 | 42.1 | 111.2 | 111.2 |
| vector.insert front | 46.1 | 46.0 | 46.2 | 46.5 | 25.5 | 24.1 |
| vector.push String | 46.4 | 16.8 | 16.0 | 26.2 | 18.4 | 9.5 |
| string.small | 268.4 | 88.6 | 85.0 | 87.1 | 74.1 | 84.9 |
| string.vector copy | 261.2 | 22.2 | 18.4 | 28.9 | 9.8 | 4.9 |
| string.append char | 17.6 | 13.3 | 10.5 | 12.1 | 5.2 | 14.5 |
| string.append pieces | 14.9 | 13.1 | 13.0 | 13.6 | 10.2 | 8.7 |
| string.concat | 27.9 | 28.7 | 22.8 | 20.3 | 7.2 | 7.5 |
| string.find | 59.1 | 6.8 | 6.7 | 1.7 | 5.9 | 6.5 |
| string.find char | 36.6 | 2.6 | 3.4 | 1.2 | 0.9 | 0.9 |
| string.compare | 29.9 | 20.8 | 14.1 | 11.8 | 8.2 | 12.4 |
| sort.int | 43.2 | 43.1 | 43.0 | 48.6 | 39.1 | 38.8 |
| sort.int predicate | > 60 s | 43.2 | 43.0 | 47.9 | 40.4 | 38.9 |
| sort.String | 318.9 | 164.9 | 121.6 | 140.7 | 142.4 | 126.3 |
| hash.int insert | 29.7 | 32.6 | 30.9 | 112.9 | 77.7 | 91.1 |
| hash.int lookup | 55.9 | 85.7 | 78.4 | 248.5 | 87.7 | 104.2 |
| hash.String insert | 341.2 | 79.9 | 71.7 | 246.9 | 212.2 | 224.0 |
| hash.String lookup | 335.6 | 97.1 | 87.0 | 164.9 | 133.5 | 148.3 |
| map.insert | 179.6 | 179.3 | 174.0 | 12.5 | 11.2 | 10.4 |
| map.lookup | 71.2 | 70.1 | 67.8 | 68.2 | 95.1 | 61.8 |
| map.iterate | 1.5 | 1.8 | 1.8 | 14.9 | 8.1 | 5.7 |

Sizes: 10M ints, 1M strings of 5-11 characters ("name123456"), a 1 MB haystack, 100k map entries;
see bench.kpp for each loop.

## What the C++ libraries do, and what Kpp does now

**Small-string optimization.** All three keep short strings inside the object: MSVC 15 chars in
32 bytes, libstdc++ 15 in 32, libc++ 22 in 24 (a flag bit selects the layout). Kpp's String is
now 24 bytes with 22 characters in place; the last byte holds the small length, or a marker for
a heap block, and all-zero bytes are the empty string. Unlike libstdc++ (which points into
itself) nothing points into the object, so Strings stay bitwise relocatable. Result:
string.small 268 -> 89 ms, string.vector copy 261 -> 22 ms (copying a small String is one
24-byte copy), vector.push String 46 -> 17 ms, hash.String 4x faster (the keys no longer
allocate). String also got `length()`, `capacity()`, `assign()` and `substr(start)`, and
`substr(start, npos)` now means "to the end" (it returned an empty string).

**Growth.** libstdc++ and libc++ double, MSVC grows by 1.5x; all reserve exactly. Kpp doubles
(from 4 elements, 22 characters) and reserve() is now exact (it rounded up to a power of two).
Out-of-line growth paths: the slow path of String::append is `[[noinline]]`, which let the fast
path inline into callers (string.append char 17.6 -> 10.5 ms in one module).

**Moves instead of copies.** HashMap/HashSet rehashed by copy-constructing every key and
destroying the old one (one malloc per String key per rehash); they now move them, like every
container (design.md section 4: move constructor, then destructor, as in C++). That is most of
hash.String's 4x. For a while Vector and the rehash copied bytes without calling constructors at
all (what the "Kpp" column above measures); kovarex decided containers keep C++'s expectations
and call the move constructor. Measured with run.cmd against that byte-copying version
(2026-10-28, ms):

| benchmark | bytes copied | move constructor |
|---|---:|---:|
| vector.push int | 13.4 | 15.1 |
| vector.push String | 16.8 | 19.6 |
| hash.String insert | 76.7 | 84.2 |
| map.insert (sorted vector) | 174.1 | 216.1 |

Everything else stayed within noise. For String the move is a 24-byte copy plus clearing the
source, and the destructor's check of the moved-from string; still faster than the MSVC STL
(26.2 and 237.6 ms).

**Trivially copyable fast paths.** The C++ libraries dispatch to memcpy/memset for trivially
copyable types. Kpp does not need a trait for it: copy, resize and insert now construct elements
in a plain loop over the destination (instead of one push_back per element, with its capacity
check), and clang turns that loop into memcpy/memset for ints. vector.copy int 82 -> 42 ms,
vector.resize 11 -> 2.7 ms, the same as the MSVC STL.

**Searching.** All three use memchr for single characters and to find candidates for the first
byte of a substring (MSVC STL also has a vectorized substring search, hence its 1.7 ms). Kpp's
String::find did one memcmp per position; now it is memchr + memcmp like libstdc++/libc++:
string.find 59 -> 6.8 ms, find char 37 -> 2.6 ms.

**Sorting.** All three use introsort. Kpp's `sort(first, last)` was quicksort without a depth
limit (quadratic on adversarial input) and `sort(first, last, less)` was an insertion sort,
which never finished on 1M elements. Both are now one introsort (median of three, unguarded
partition, heapsort past 2 log n levels, final insertion sort) and on par with the C++ ones.
`stableSort` did not exist although the converter maps std::stable_sort to it (three call sites
in converted Factorio code); it is now a merge sort of insertion-sorted runs that relocates
elements bitwise between the range and one buffer.

**Hash tables, now.** HashMap and HashSet keep each entry in a node of its own, as the
specification requires (references survive growth, like std::unordered_map's), behind the same
open-addressing table with a byte of hash bits per slot, so a lookup that misses rarely touches a
node. FastHashMap and FastHashSet keep the entries in the table: 2-4x faster than every C++
library's unordered_map in the table above, with the weaker invalidation rule. The rest of this
paragraph describes the first pass, on the flat table.

**Hash tables.** The std unordered containers are node-based (one allocation per element,
pointer chasing on lookup), which is why every flat open-addressing table beats them. Kpp's
HashMap already was one; it now also probes once per insert (operator[] used to probe, then
probe again to insert), reuses deleted slots, turns a deleted slot back into an empty one when
the next is empty, and rehashes at the same size when deleted entries make up the load (before,
insert/erase churn doubled the table on every rehash, without bound). Copies keep slot positions
(and deleted markers, which later keys' probe paths go through) instead of reinserting, and
HashMap got its missing move constructor and assignment.
Hashing: integers were multiplied by 2^64/phi and the low bits taken, so keys that are multiples
of 16 (aligned pointers) used one slot in 16. `hashBits` now folds the high half of the product
into the low half. That costs the sequential-key case below (hash.int lookup 56 -> 86 ms: keys
i*7 used to land in consecutive slots without any collision, now they are spread like random
keys) but removes the clustering for pointers; still 3x faster than MSVC's unordered_map and on
par with libstdc++'s. Strings were hashed with byte-at-a-time FNV-1a (MSVC uses the same, libstdc++
murmur2, libc++ murmur2/cityhash); `hashBytes` now takes eight bytes per step with a murmur3
finalizer.

**Aliasing safety.** Fixed on the way: `v.push_back(v[0])` and `s.append(s)` read freed memory
when they had to grow, and `v.insert(i, v[j])` read a moved element. The growth paths now build
the new element before moving the old ones, as the C++ libraries do, and HashMap copies a key
that has to survive a rehash.
compiler/tests/run/containers2.kpp covers these, the small/large switch and the hash table churn.

**Ordered maps.** All three C++ libraries implement std::map and std::set as red-black trees:
one allocation per entry and one pointer to chase per level. Kpp's Map and Set were sorted
Vectors (fast lookups and iteration, but O(n) inserts: 100k inserts took 179 ms against 12.5 for
std::map). Map, Set, MultiMap and MultiSet are now red-black trees too (stdlib/core/Map.kpp), with
the same guarantees as std's (entries never move; see docs/stdlib-conformance.md). FastMap and
FastSet (FastMap.kpp) are the explicitly faster alternative: a B-tree in the layout of Abseil's
btree_map, nodes of up to 15 entries, split on the way down when inserting and refilled from a
sibling or merged on the way down when erasing. Keys of up to 8 bytes are searched linearly within
a node, which beat binary search there by 40%; larger keys binary. The price: entries move between
nodes, so every insert and erase invalidates iterators and references. Measured with run.cmd
2026-10-28, ms (columns as above):

| benchmark | sorted Vector | Map (red-black) | FastMap (B-tree) | MSVC STL | libstdc++ | libc++ |
|---|---:|---:|---:|---:|---:|---:|
| map.insert | 179.3 | 14.2 | 8.9 | 12.8 | 11.4 | 10.8 |
| map.lookup | 70.1 | 73.1 | 57.8 | 70.3 | 93.6 | 60.7 |
| map.iterate | 1.8 | 17.6 | 7.3 | 15.5 | 8.0 | 5.6 |

**Prefetching, allocators.** None of the three C++ libraries prefetches explicitly in these
containers; the hardware prefetcher handles the linear scans. Allocator strategy matters more: the
libstdc++ and libc++ columns are faster on string.concat and string.vector copy mostly because
glibc malloc is faster than the Windows heap for small blocks, not because of the library. A Kpp
allocator of its own (size-class free lists per thread, as in mimalloc) would give that on
Windows too, and would make the timings independent of the heap's history; it is the largest
library-level item left.

## Gaps left

Library design:
- **Allocator**: see above (string.concat 22.8 ms against 7.2 with glibc's malloc).
- **vector.insert front**: memmove of the tail, same as MSVC STL; libstdc++/libc++ are faster
  only through glibc's memmove.

Compiler codegen:
- **No inlining across IR modules.** kppc defines each non-template function in one module, so
  String's small methods are calls everywhere else; compare the "Kpp" and "Kpp, 1 module"
  columns: string.compare 20.8 vs 14.1 ms, sort.String 164.9 vs 121.6, string.concat 28.7 vs
  22.8, string.append char 13.3 vs 10.5, hash.String 10%. C++ has every std function in headers,
  so the C++ columns are the "1 module" case. Fixes: emit small functions as
  `available_externally` copies into the modules that call them, or ThinLTO for `--release`.
- Within one module the Kpp code is at or near clang/C++ quality for these loops (vector.iterate,
  sort.int, vector.copy are equal to the MSVC STL).
