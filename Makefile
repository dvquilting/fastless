CC ?= cc
PKG_CONFIG ?= pkg-config
CARGO ?= cargo
CARGO_FLAGS ?= --locked
CFLAGS ?= -O3 -DNDEBUG
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags zlib libzstd liblzma 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs zlib libzstd liblzma 2>/dev/null || echo '-lz -lzstd -llzma')

fastless: fastless.o input.o literal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

literal-tests: literal_tests.o literal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

# Tiny search pieces push even the small test files through the threaded search.
fastless-tiny: fastless-tiny.o input.o literal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

fastless-tiny.o: fastless.c
	$(CC) $(CPPFLAGS) $(CFLAGS) '-DSEARCH_NEAR=((size_t)40)' '-DSEARCH_PIECE=((size_t)24)' -c -o $@ $<

fastless.o fastless-tiny.o: input.h literal.h
input.o: input.h
literal.o literal_tests.o: literal.h

check: fastless fastless-tiny literal-tests
	./literal-tests
	python3 tests.py
	FASTLESS_TEST_SIMD=off python3 tests.py
	python3 compression_tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/fastless-tiny" python3 tests.py

rust:
	$(CARGO) build --manifest-path rust/Cargo.toml --release $(CARGO_FLAGS)

check-rust: rust
	$(CARGO) test --manifest-path rust/Cargo.toml --release $(CARGO_FLAGS)
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/release/fastless-rs" python3 tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/release/fastless-rs" FASTLESS_TEST_SIMD=off python3 tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/release/fastless-rs" python3 compression_tests.py
	RUSTFLAGS='--cfg fastless_tiny_pieces' $(CARGO) build --manifest-path rust/Cargo.toml --release $(CARGO_FLAGS) --target-dir rust/target/tiny
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/tiny/release/fastless-rs" python3 tests.py

compare: fastless rust
	python3 compare_versions.py

clean:
	rm -f fastless fastless-tiny literal-tests *.o

.PHONY: check check-rust rust compare clean
