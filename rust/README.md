# fastless-rs

A separate Rust implementation of fastless with the same command-line modes,
POSIX regex semantics, case handling, paging, highlighting, and compressed-file
support as the C implementation.

```sh
# From the fastless directory:
make rust
./rust/target/release/fastless-rs huge.log
./rust/target/release/fastless-rs --literal -I huge.log.zst
./rust/target/release/fastless-rs --tmpdir /fast/disk huge.log.xz
make check-rust
```

Or build directly:

```sh
cargo build --manifest-path rust/Cargo.toml --release --locked
```

Requires Rust 1.77+, the libc crate, and the same installed zlib, libzstd, and
liblzma libraries as the C version. The lockfile pins the Rust dependency.
`pkg-config` supplies native linker paths; `PKG_CONFIG` and `PKG_CONFIG_PATH`
are respected. On a host with the Rust dependency already cached, use
`make rust CARGO_FLAGS='--locked --offline'`.

The pager, mmap lifetime, temporary-file management, decode loop, glob matcher,
literal matcher, and AVX2 kernels are Rust code. No C fastless implementation
or C shim is linked. POSIX regex and the compression engines use native-library
FFI, preserving behavior and making performance comparisons less dependent on
changing algorithms/libraries. The small native codec ABI bindings are isolated
in `src/codecs.rs`; platform libc layouts come from the libc crate.

The same 8 MiB decode buffers, xz thread/memory settings, literal skip tables,
vector candidate filters, dense-candidate fallback, and conservative regex-prefix
filters are used. RAII restores terminal settings and releases mappings/decoder
state on normal return and unwinding. Decoder state is boxed at a stable address
and dropped before its borrowed buffers/mappings. Sources must stay stable while
mapped; external in-place truncation can still cause SIGBUS.

`--simd auto|off|avx2` has the same meaning as in C. Runtime feature detection
and per-function target features use [Rust's architecture intrinsics](https://doc.rust-lang.org/std/arch/index.html),
without globally enabling AVX2. Apple Silicon uses the portable path. Linux
has been tested; a native macOS run is still needed.

The shared Python suites verify byte offsets, sparse 200 GiB files, terminal
navigation/highlighting/restoration, concatenated and corrupt compressed streams,
and cancellation. Rust unit tests cover native ABI sizes, randomized binary
matching, inaccessible page boundaries, dense false candidates, and every byte's
case-folding behavior, with forward/reverse and off/auto/forced-AVX2 variants.
Use release-mode tests for the large differential matrices:

```sh
cargo test --manifest-path rust/Cargo.toml --release --locked
cargo clippy --manifest-path rust/Cargo.toml --release --all-targets -- -D warnings
```

See [the shared README](../README.md) for navigation, options, and limits, and
[the performance report](../PERFORMANCE.md) for measured C/Rust comparisons.
