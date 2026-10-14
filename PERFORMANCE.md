# C and Rust performance comparison

The two implementations are close on this machine: Rust was within about 7% of C on every search. With default threading, every search on the 256 MiB fixtures, including regex and glob, reached roughly 14–16 GiB/s. At that point the time is dominated by memory bandwidth, page mapping, and process startup rather than matching. Limited to one thread, both reached roughly 8 GiB/s. zstd and xz input are equally fast in both; gzip input is about 1.6x faster in C, which links zlib-ng, while Rust uses the system zlib.

## Setup

- CPU: AMD Ryzen 7 5700X, AVX2 available; 8 cores, 16 logical CPUs.
- OS: Linux 7.0.0-31-generic, 64-bit.
- C: GCC 15.2.0, `-O3 -DNDEBUG -std=c11 -pthread`; runtime SIMD dispatch, no global `-march=native`/`-mavx2`.
- Rust: rustc 1.95.0; release `opt-level=3`, thin LTO, one codegen unit; runtime SIMD dispatch.
- Native backends: POSIX regex from libc, libzstd 1.5.7, and liblzma 5.8.3 in both. For gzip, C links zlib-ng 2.3.3 (static, zlib-compatible mode, from the submodule); Rust links the system zlib 1.3.1.
- 256 MiB input per fixture, match at the end; median of 7 interleaved warm-cache runs after warming/validating every variant.
- Repetitive log: 1 KiB rows, mostly repeated bytes. Mixed text: varied ASCII log records with services, paths, timestamps, numbers, and messages.
- Every timed run checks the exact expected byte offset. Compressed timings include complete decode, temporary-file writes, mmap search, and cleanup. Fixture generation is excluded.
- `off` disables each program's custom SIMD kernels; libc/native codecs may retain their own SIMD.
- Search threads default to the number of logical CPUs (16 here). The one-thread table passes `--threads 1`.
- At default threading each search takes under 20 ms, so repeated runs of the whole comparison varied by up to about 10% per row.

## Results

Throughput is GiB/s of uncompressed input. Rust/C speed above 1 favors Rust.

### Default threads

| Fixture / operation | C auto | Rust auto | Rust/C speed | C off | Rust off |
| --- | ---: | ---: | ---: | ---: | ---: |
| log / literal | 14.42 | 14.05 | 0.97x | 14.03 | 14.01 |
| log / literal -I | 15.31 | 14.43 | 0.94x | 13.28 | 12.84 |
| log / regex plain | 15.51 | 14.99 | 0.97x | 15.61 | 14.66 |
| log / regex prefix | 14.82 | 14.90 | 1.01x | 15.23 | 14.68 |
| log / regex general | 14.72 | 14.58 | 0.99x | 14.42 | 13.82 |
| log / glob simple | 15.27 | 14.38 | 0.94x | 15.04 | 15.16 |
| log / glob general | 15.49 | 14.64 | 0.95x | 14.84 | 14.19 |
| mixed / literal | 14.73 | 13.70 | 0.93x | 14.31 | 13.76 |
| mixed / literal -I | 15.35 | 14.49 | 0.94x | 12.29 | 12.59 |
| mixed / regex plain | 15.69 | 15.05 | 0.96x | 15.12 | 14.61 |
| mixed / regex prefix | 14.81 | 14.40 | 0.97x | 14.69 | 14.08 |
| mixed / regex general | 14.41 | 13.91 | 0.97x | 13.23 | 12.79 |
| mixed / glob simple | 15.12 | 15.03 | 0.99x | 14.89 | 14.44 |
| mixed / glob general | 15.36 | 14.78 | 0.96x | 13.89 | 13.71 |
| mixed compressed / gzip decode+literal | 1.12 | 0.70 | 0.62x | — | — |
| mixed compressed / zstd decode+literal | 1.17 | 1.16 | 0.99x | — | — |
| mixed compressed / xz decode+literal | 1.38 | 1.42 | 1.02x | — | — |

### One search thread

| Fixture / operation | C auto | Rust auto | Rust/C speed | C off | Rust off |
| --- | ---: | ---: | ---: | ---: | ---: |
| log / literal | 8.38 | 8.19 | 0.98x | 7.62 | 7.94 |
| log / literal -I | 8.35 | 8.29 | 0.99x | 3.29 | 3.12 |
| log / regex plain | 8.39 | 8.24 | 0.98x | 8.03 | 7.26 |
| log / regex prefix | 8.34 | 8.14 | 0.98x | 7.71 | 7.67 |
| log / regex general | 8.28 | 8.28 | 1.00x | 6.28 | 6.20 |
| log / glob simple | 8.51 | 8.38 | 0.99x | 8.04 | 8.04 |
| log / glob general | 8.06 | 8.24 | 1.02x | 5.42 | 5.48 |
| mixed / literal | 7.97 | 7.82 | 0.98x | 6.19 | 5.81 |
| mixed / literal -I | 7.86 | 7.74 | 0.99x | 2.83 | 2.69 |
| mixed / regex plain | 8.03 | 7.80 | 0.97x | 6.13 | 6.10 |
| mixed / regex prefix | 8.22 | 8.24 | 1.00x | 6.81 | 6.80 |
| mixed / regex general | 7.79 | 8.30 | 1.07x | 5.86 | 5.82 |
| mixed / glob simple | 7.95 | 7.83 | 0.98x | 6.17 | 6.13 |
| mixed / glob general | 8.26 | 8.31 | 1.01x | 4.10 | 4.19 |

### Compared with the previous version

C build, 4 GiB mixed-text fixture with the match at the end, warm cache. "Previous" is commit f7dd580, which searched on one thread and only used a literal prefix to filter regex lines.

| Search | Previous | Now, 1 thread | Now, 16 threads |
| --- | ---: | ---: | ---: |
| literal | 0.45 s | 0.46 s | 0.24 s |
| regex `NEEDLE` | 2.11 s | 0.46 s | 0.24 s |
| regex `status=5[0-9]7` (prefix on 1 line in 4) | 3.14 s | 1.92 s | 0.33 s |
| regex `[0-9]+qq` | 35.88 s | 0.46 s | 0.24 s |
| regex `z+Aq` | 3.19 s | 0.48 s | 0.24 s |
| glob `*zz?qq*` | 8.79 s | 0.49 s | 0.24 s |
| glob `*NEEDLE*` | 2.18 s | 0.46 s | 0.24 s |

A regex with no extractable literal, `[0-9]+[q]` on a 1 GiB fixture, took 8.99 s on one thread and 1.34 s on eight (6.7x); 16 threads gave no further gain. Rust measured 9.22 s and 1.37 s.

Decoding plus a literal search of a 2 GiB file, C build before the switch to zlib-ng: zstd 2.71 → 2.08 s, gzip 4.27 → 3.55 s, xz 1.60 → 1.45 s. Rust measured 2.04, 3.54, and 1.43 s. For reference, the `zstd` tool alone decompressed the same file in 1.77 s.

### gzip with zlib-ng

The C build now links zlib-ng 2.3.3 (zlib-compatible mode, static) from a submodule. Decoding plus a literal search of a 2.16 GB log file, median of 5 interleaved warm-cache runs:

| Compression | zlib 1.3.1 | zlib-ng 2.3.3 | Speedup |
| --- | ---: | ---: | ---: |
| `gzip -6` (466 MB) | 3.61 s | 2.09 s | 1.73x |
| `gzip -1` (602 MB) | 3.61 s | 1.99 s | 1.81x |

For reference, `gzip -dc` and `pigz -dc` to `/dev/null` took 3.6–4.3 s and 3.45 s. The Rust build still links the system zlib, which is why its gzip row in the default-thread table is about 0.6x of C.

## Interpretation

Two changes account for most of the regex and glob gains. A required literal is extracted from each regex or glob, and only lines containing it reach the full matcher. Searches reaching more than 8 MiB from their start are then split into 4 MiB pieces across threads. With a rare literal, every search mode runs at literal-scan speed. The "general" rows above all contain such a literal; patterns with alternation or without any literal still call the regex engine on every line, which is where threads help most.

Threaded literal scans level off near 16–20 GiB/s after about four threads. Memory bandwidth, page-fault handling for the mapping, and process exit costs set that ceiling, so the AVX2 kernels and the portable path converge except for case insensitive literals. Simultaneous multithreading adds nothing for search on this CPU.

On one thread, AVX2 is most useful for case insensitive literal matching, improving it from about 3.3 to 8.3 GiB/s in both versions. Short case sensitive line searches often keep the optimized libc path.

Compressed input gains come from a writer thread that appends decoded buffers to the temporary file while the next buffer is decoded. gzip remains limited by single-threaded inflate, even with zlib-ng. Full compressed-file opening still pays for decoding and temporary storage; neither SIMD nor search threads remove those costs.

Differences of a few percent should not be treated as a universal language ranking. These are single-machine warm-cache measurements with different compiler pipelines; storage speed, data distribution, pattern selectivity, line lengths, CPU frequency, core count, and compression settings all change the result. A cold search is usually limited by storage. The comparison fixtures are 256 MiB, not full 1–200 GB scans, so process startup is a noticeable share of the threaded times. A sparse 200 GiB integration test verifies addressing without measuring whole-file throughput. No native macOS benchmark has been run.

## Validation

- C: 362,280 literal differential checks, including guarded page boundaries, every byte, overlaps, dense candidates, and both directions; GCC/Clang, SIMD off/auto/forced, and ASan/UBSan validation. Threaded search and the decoder writer thread were also run under ThreadSanitizer.
- Rust: four release-mode unit groups covering ABI layouts and equivalent literal differential matrices; Clippy passes with warnings denied.
- Both: the same 199 pager/search checks and 81 compression checks pass; portable/off integration is also checked. A second build of each uses 24-byte search pieces so the small test files exercise threaded search.
- Randomized forward and backward searches were compared against the previous version's output with 1, 3, and 16 threads.

## Reproduce

```sh
git submodule update --init
make -j
make rust
make -j check
make check-rust
python3 compare_versions.py --mib 256 --runs 7 --compressed --json performance-results.json
```

Use `--tmpdir DIR` to select benchmark storage, or `--c PATH` / `--rust PATH` for other builds; the one-thread table used wrapper scripts that add `--threads 1`. Larger `--mib` values reduce process-startup effects. Results and individual samples for the default-thread table are in [performance-results.json](performance-results.json).
