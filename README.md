# fastless

A small, 64-bit C pager for **regular files**, designed for searching 1–200 GB
files, including gzip, zstd, and xz input. It maps plain files read-only;
compressed input is decoded into a temporary file and then mapped read-only.
It never constructs a line index or copies the whole file into RAM. Linux and
macOS are supported targets.

```sh
make
./fastless huge.log
./fastless --literal huge.log
./fastless --glob huge.log
LESS='-i' ./fastless huge.log
```

Builds with GCC or Clang and links zlib, libzstd, and liblzma. `pkg-config`
provides their compiler/linker flags, with standard library names as a fallback.
Requires a 64-bit process and POSIX `REG_STARTEND` support (Linux and macOS).
Each source file compiles separately, so `make -j` builds in parallel.

```sh
# Debian / Ubuntu build dependencies
sudo apt install build-essential pkg-config zlib1g-dev libzstd-dev liblzma-dev

# macOS with Homebrew
brew install pkgconf zlib zstd xz
PKG_CONFIG_PATH="$(brew --prefix zlib)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" make PKG_CONFIG=pkgconf
```

Homebrew formula details: [pkgconf](https://formulae.brew.sh/formula/pkgconf),
[zlib](https://formulae.brew.sh/formula/zlib),
[zstd](https://formulae.brew.sh/formula/zstd),
[xz](https://formulae.brew.sh/formula/xz).

`make check` (or `make -j check`) runs the Python 3 integration suite, including a sparse 200 GiB
file, real pseudo-terminal output, compressed streams, and cancellation cleanup.
It also compares AVX2 and portable literal kernels against a bytewise reference,
including inaccessible page boundaries, all byte values, overlapping matches,
and both search directions. Pager integration runs with SIMD auto and off,
and again with a build using tiny search pieces so that the small test files
exercise the threaded search.
Python 3 tests use its gzip/lzma modules and libzstd via ctypes (libzstd 1.4+).
The optional threaded-xz fixture also uses the `xz` executable if installed.
Linux has been tested locally; macOS still needs a native build/test run.

## Rust implementation and comparisons

A separate Rust version implements the same pager, mmap/decode handling,
highlighting, search modes, and runtime AVX2 dispatch:

```sh
make rust
./rust/target/release/fastless-rs --literal -I huge.log.zst
make check-rust
python3 compare_versions.py --mib 256 --runs 7 --compressed
```

Rust requires Rust 1.77+ and the libc crate in addition to the existing native
codec libraries. See [rust/README.md](rust/README.md) for build details and
[PERFORMANCE.md](PERFORMANCE.md) for the measured comparison and its limitations.
The Rust implementation does not link the C pager or scanner; both versions
use the same POSIX regex and compression libraries through their native APIs.

## Compressed input

```sh
./fastless huge.log.gz
./fastless huge.log.zst
./fastless huge.log.xz
./fastless --literal --find 'needle' huge.log.zst
./fastless --tmpdir /path/to/fast/disk huge.log.xz
./fastless --threads 4 huge.log.xz
```

Format detection uses magic bytes, so filenames/extensions do not matter.
`--compression auto|none|gzip|zstd|xz` overrides detection; `none` opens bytes
exactly as stored. gzip members, zstd frames (including skippable frames), and
xz streams may be concatenated. Streams are fully decoded and their supported
integrity checks verified before paging or returning search results. Corrupt,
truncated, or unsupported streams produce an error rather than partial results.
External zstd dictionaries are not supported.

Temporary files are created with mode 0600 in `--tmpdir DIR`, otherwise `$TMPDIR`
or `/tmp`, and unlinked immediately. The mapping keeps their storage alive until
the pager exits; exiting, cancelling, or killing the process releases it. There
is no persistent decompression cache. You need free space for the **full decoded
size**, which may be much larger than the compressed file. `/tmp` may be a
memory-backed filesystem on Linux; choose a disk-backed directory for large
files. The program does not fsync temporary data or modify the source file.

Decoding finishes before the first page or search is available, including when
`--offset` is specified. Offsets, navigation, and the status bar refer to decoded
bytes. Terminal stderr shows decoding progress; Ctrl-C cancels it. No pipes or
external decoder processes are used by fastless itself.

Searches reaching more than 8 MiB from their starting point are divided among
threads in 4 MiB pieces; the first match in file order is still the one
reported. A regex or glob is first reduced to the longest literal every match
must contain, and only lines holding it are matched in full.

The native C decoders fill 8 MiB output buffers that a writer thread appends to
the temporary file, with compressed input accessed through mmap. Searching and
xz decoding automatically use up to 32 logical CPUs; `--threads N` selects
1–256, or 0 for automatic.
liblzma 5.4+ supports the threaded decoder; older versions use a single thread.
Parallel decoding requires xz blocks with size metadata, usually produced by a
threaded encoder; single-block files stay single-threaded. See the
[liblzma decoder documentation](https://tukaani.org/xz/liblzma-api/container_8h.html).
gzip and standard zstd streams are decoded with one decoder thread. xz limits
thread buffering to 256 MiB and decoder memory to 512 MiB, reducing threads when
necessary. zstd uses its library's default maximum window (128 MiB). These limits
are independent of decoded file size; unusually large dictionaries/windows may
be rejected. OS page caching and mapped pages are additional memory use.

## Searching

Regular expressions are the default: POSIX extended regex, matched within
individual lines, without their trailing newline. `/` enters a forward search,
`?` a backward search. `n` repeats the original direction; `N` reverses it.
There is no automatic wrap. Search results are reported by byte offset and
positioned at the start of the matching line. All visible matches of the current
pattern are highlighted in reverse video, including overlapping matches. Regex
and literal modes highlight matching bytes; glob mode highlights the whole
matching line. Zero-length regex matches have no bytes to highlight. Highlighting
follows the active case-sensitivity setting and updates as you navigate.

Switch modes with `r` (regex), `l` (literal), or `w` (glob). Switching modes
recompiles the current pattern; an invalid regex leaves the old mode active.
An empty search prompt reuses the current pattern.

* **Literal:** substring matching, including across newline boundaries. AVX2
  accelerates eligible forward/backward searches. The portable path uses the
  system's `memmem` for case sensitive forward searches, or skip tables for
  case insensitive and backward searches. Boundary matches are retained.
* **Glob:** matches the **whole line**. `*` matches any number of bytes, `?`
  matches one byte, and backslash quotes the next character. For a substring
  search, use `*text*`; simple `*literal*` patterns use the fast literal matcher,
  and other patterns are only matched on lines holding their longest literal run.
  Bracket classes are not supported. No line allocation.
* **Regex:** POSIX ERE, such as `^ERROR.*timeout`, without PCRE extensions.
  Uses `REG_STARTEND` against the mapping, without copying lines. A single
  line over 2 GiB produces an error; use literal or glob mode for such a line.
  Plain literal regexes use the fast matcher within each line. Otherwise the
  longest top-level literal run that every match must contain is found with
  the fast matcher, and the regex runs only on lines containing it. Alternation
  disables this, and characters followed by `*`, `+`, `?`, or `{` are not part
  of a run; patterns without a usable run use the full regex path.

`LESS` option clusters such as `-FRiX` are inspected for `-i` and `-I`;
other LESS features are not implemented. `-i` ignores case only when the
pattern contains no ASCII uppercase letters (less's smart-case behavior).
`-I` always ignores case. Explicit command-line options override `LESS`;
`--case-sensitive` disables inherited case folding. Matching uses the C
locale: case folding is ASCII, not Unicode. Files and patterns are bytes.

## Navigation

| Key | Action |
| --- | --- |
| Space / `f` | Next page |
| `b` | Previous page |
| `j` / Enter | Next line |
| `k` | Previous line |
| `g` / `G` | Beginning / last page |
| `/` / `?` | Forward / backward search |
| `n` / `N` | Repeat / reverse search |
| `r` / `l` / `w` | Regex / literal / glob |
| `q` | Quit |
| Ctrl-C | Cancel search |

Long lines are clipped to terminal width; there is no horizontal scrolling.
Tabs expand to eight-column stops. Non-ASCII and control bytes display as
`.` so file contents cannot inject terminal escape sequences. No UTF-8
rendering, stdin/pipe input, follow mode, or line numbers.
The pager uses the alternate screen and restores terminal settings on exit.
Patterns entered interactively are limited to 4095 ASCII bytes. Arrow keys
and job-control suspension are not implemented; use the navigation keys above.

## Noninteractive searches

```sh
# Print the byte offset of the first match; regex is the default.
./fastless --find '^ERROR.*timeout' huge.log

# Print every overlapping literal match.
./fastless --literal --find 'needle' --all huge.log

# Start near an already known position without scanning the preceding file.
./fastless --literal --find 'needle' --offset 150000000000 huge.log

./fastless --glob --find '*timeout*' --all huge.log
```

Offsets are zero-based. `--all` includes overlapping and zero-length regex
matches, advancing at least one byte after each result. Glob output gives
one offset per matching line. Empty patterns are rejected; a regex such as
`^$` can match an empty line. Exit codes: 0 match/success, 1 no match, 2 error,
130 interrupted search. Use `--` before a filename beginning with `-`.

## AVX2 search acceleration

256-bit AVX2 instructions are enabled automatically on supported x86 CPUs.
Runtime CPU detection uses the compiler's
[CPU feature built-ins](https://gcc.gnu.org/onlinedocs/gcc/x86-Built-in-Functions.html).
The build does **not** require `-mavx2` or `-march=native` globally: non-AVX2 x86
CPUs and Apple Silicon keep the portable implementation.

```sh
./fastless --literal -I huge.log                 # automatic dispatch
./fastless --simd off --literal -I huge.log      # disable custom AVX2 kernels
./fastless --simd avx2 --literal -I huge.log     # force eligible AVX2 kernels
```

`--simd auto|off|avx2` selects runtime dispatch; auto is the default. `avx2`
reports an error if unsupported. `off` disables fastless's custom SIMD code;
the system C/compression libraries may still use their own CPU acceleration.

The vector matcher filters 32 candidate positions at a time using two pattern
bytes, batches four vectors for larger forward scans, then verifies survivors.
ASCII case insensitive filtering handles letters without changing punctuation
or high-bit bytes. Short tails are scalar; vector loads stay within the supplied
range, including at mmap/page boundaries. Dense false positives switch back to
the portable matcher. Patterns longer than 256 bytes use the portable algorithms.
Auto mode retains libc for short case sensitive forward ranges, where it won
in local measurements. No whole-file folding/copying or extra file index is used.

AVX2 accelerates literal matching and the literal parts of eligible glob/regex
searches. Arbitrary regex evaluation and decompression remain in their native
libraries; storage I/O can still dominate a cold search.

Reproduce warm-cache comparisons with:

```sh
python3 benchmark.py --mib 512 --compare
# Optional: include an older binary as a baseline.
python3 benchmark.py --mib 512 --compare --baseline /path/to/older/fastless
```

Variants are warmed and interleaved across five runs, reporting median GiB/s.
The fixture is 1 KiB log lines with a match at the end. On the local Ryzen 7
5700X, case insensitive literal search measured approximately 8.2 GiB/s versus
3.3 GiB/s in the previous binary (about 2.5x); case sensitive literal search
measured approximately 8.3 versus 7.7 GiB/s. Prefix-regex and simple-glob
improvements also include the new algorithms, not just SIMD. These figures
are specific to this data and warm cache; they are not storage-speed guarantees.

## Performance and limits

Literal mode is the preferred path for maximum throughput. Regex mode must
identify each line and call the platform regex engine; complex patterns can
be much slower, though long searches use several threads. Backward regex search finds the final match on each candidate
line and can be expensive on lines with many matches. Glob wildcard patterns
and case insensitive literal patterns can also have unfavorable worst cases.
Ctrl-C is checked between work units; an individual regex-engine call or a
very long line scan may delay cancellation.

The mapping reserves virtual address space equal to the file size, while the
OS loads pages on demand. A cold search still reads the scanned bytes from
storage; mmap does not remove that I/O cost. Resident memory and page tables
can grow as the OS caches scanned pages. Heap use is independent of file size,
apart from regex-engine internal allocations and a regex copy per search
thread. No persistent index or speculative whole-file prefetch is performed.

Threads help most with regex and glob patterns that have no usable literal; on
the local 8-core Ryzen, such a regex ran about 6.7 times faster with eight
threads. Literal-speed scans roughly double before memory bandwidth limits them,
and threads beyond the physical core count added nothing. `--threads 1` keeps
searches on the calling thread. See [PERFORMANCE.md](PERFORMANCE.md).

Keep the mapped file stable while viewing it. In-place truncation can cause
`SIGBUS`, as with other mmap readers. Replacing its pathname does not update
the existing mapping. Compressed source files must also stay stable during
decoding. Only compression is removed; archive entries such as tar members
are not extracted.

`python3 benchmark.py --mib 256` measures first-match throughput on a generated
file. This is a warm-cache microbenchmark, not a storage-speed prediction.

`python3 compression_benchmark.py --mib 256 --runs 3` measures end-to-end
compressed-file opening plus literal search, compared with the corresponding
CLI decoder writing a file followed by fastless. Fixture generation is excluded;
requires the gzip, zstd, and xz executables. `--tmpdir DIR` selects the benchmark's
storage. Results depend strongly on data, compression settings, and storage.
