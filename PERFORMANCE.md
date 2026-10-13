# C and Rust performance comparison

The two implementations are close on this machine. With automatic SIMD, Rust was within about 5% of C on most searches. Both reached roughly 8 GiB/s on warm-cache literal searches. General glob matching favored Rust by about 14% on the repetitive-log fixture and was essentially equal on mixed text. Compressed-file measurements were also close.

## Setup

- CPU: AMD Ryzen 7 5700X, AVX2 available; 16 logical CPUs.
- OS: Linux 7.0.0-31-generic, 64-bit.
- C: GCC 15.2.0, `-O3 -DNDEBUG -std=c11`; runtime SIMD dispatch, no global `-march=native`/`-mavx2`.
- Rust: rustc 1.95.0; release `opt-level=3`, thin LTO, one codegen unit; runtime SIMD dispatch.
- Shared native backends: POSIX regex from libc, zlib 1.3.1, libzstd 1.5.7, liblzma 5.8.3.
- 256 MiB input per fixture, match at the end; median of 7 interleaved warm-cache runs after warming/validating every variant.
- Repetitive log: 1 KiB rows, mostly repeated bytes. Mixed text: varied ASCII log records with services, paths, timestamps, numbers, and messages.
- Every timed run checks the exact expected byte offset. Compressed timings include complete decode, temporary-file writes, mmap search, and cleanup. Fixture generation is excluded.
- `off` disables each program's custom SIMD kernels; libc/native codecs may retain their own SIMD.

## Results

Throughput is GiB/s of uncompressed input. Rust/C speed above 1 favors Rust.

| Fixture / operation | C auto | Rust auto | Rust/C speed | C off | Rust off |
| --- | ---: | ---: | ---: | ---: | ---: |
| log / literal | 8.22 | 8.04 | 0.98x | 7.56 | 6.73 |
| log / literal -I | 8.37 | 8.16 | 0.97x | 3.27 | 3.11 |
| log / regex plain | 6.45 | 6.22 | 0.97x | 6.50 | 6.38 |
| log / regex prefix | 6.16 | 6.13 | 1.00x | 6.10 | 6.11 |
| log / regex general | 2.43 | 2.42 | 1.00x | 2.42 | 2.42 |
| log / glob simple | 6.36 | 6.16 | 0.97x | 6.37 | 6.26 |
| log / glob general | 0.58 | 0.66 | 1.14x | 0.58 | 0.67 |
| mixed / literal | 8.08 | 7.76 | 0.96x | 6.21 | 6.08 |
| mixed / literal -I | 7.68 | 7.61 | 0.99x | 2.79 | 2.71 |
| mixed / regex plain | 2.67 | 2.58 | 0.96x | 2.69 | 2.57 |
| mixed / regex prefix | 2.84 | 2.73 | 0.96x | 2.89 | 2.73 |
| mixed / regex general | 0.85 | 0.84 | 0.99x | 0.85 | 0.84 |
| mixed / glob simple | 2.62 | 2.55 | 0.97x | 2.64 | 2.53 |
| mixed / glob general | 0.50 | 0.49 | 0.99x | 0.50 | 0.49 |
| mixed compressed / gzip decode+literal | 0.55 | 0.56 | 1.02x | — | — |
| mixed compressed / zstd decode+literal | 0.87 | 0.86 | 0.99x | — | — |
| mixed compressed / xz decode+literal | 1.23 | 1.22 | 1.00x | — | — |

## Interpretation

AVX2 is most useful for case insensitive literal matching. On repetitive logs it improves C from about 3.3 to 8.4 GiB/s and Rust from about 3.1 to 8.2 GiB/s; mixed-text results show a similar gain. Short case sensitive line searches often keep the optimized libc path. Literal-prefix regex and simple-glob gains also include algorithm changes, not only SIMD.

General regex uses the same native engine in both versions and has nearly identical throughput. More frequent line boundaries in mixed text add per-line work, lowering regex/glob throughput relative to long repetitive rows. Full compressed-file opening adds decoding and temporary-storage costs; SIMD cannot eliminate those costs.

Differences of a few percent should not be treated as a universal language ranking. These are single-machine warm-cache measurements with different compiler pipelines; storage speed, data distribution, pattern selectivity, line lengths, CPU frequency, and compression settings all change the result. The fixtures are 256 MiB, not full 1–200 GB scans. A sparse 200 GiB integration test verifies addressing without measuring whole-file throughput. No native macOS benchmark has been run.

## Validation

- C: 362,280 literal differential checks, including guarded page boundaries, every byte, overlaps, dense candidates, and both directions; GCC/Clang, SIMD off/auto/forced, and ASan/UBSan validation.
- Rust: four release-mode unit groups covering ABI layouts and equivalent literal differential matrices; Clippy passes with warnings denied.
- Both: the same 199 pager/search checks and 81 compression checks pass; portable/off integration is also checked.

## Reproduce

```sh
make
make rust
make check
make check-rust
python3 compare_versions.py --mib 256 --runs 7 --compressed --json performance-results.json
```

Use `--tmpdir DIR` to select benchmark storage, or `--c PATH` / `--rust PATH` for other builds. Larger `--mib` values reduce process-startup effects. Results and individual samples are in [performance-results.json](performance-results.json).
