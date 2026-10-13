CC ?= cc
PKG_CONFIG ?= pkg-config
CARGO ?= cargo
CARGO_FLAGS ?= --locked
CFLAGS ?= -O3 -DNDEBUG
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags zlib libzstd liblzma 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs zlib libzstd liblzma 2>/dev/null || echo '-lz -lzstd -llzma')

fastless: fastless.c input.c input.h literal.c literal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ fastless.c input.c literal.c $(LDLIBS)

literal-tests: literal_tests.c literal.c literal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ literal_tests.c literal.c

check: fastless literal-tests
	./literal-tests
	python3 tests.py
	FASTLESS_TEST_SIMD=off python3 tests.py
	python3 compression_tests.py

rust:
	$(CARGO) build --manifest-path rust/Cargo.toml --release $(CARGO_FLAGS)

check-rust: rust
	$(CARGO) test --manifest-path rust/Cargo.toml --release $(CARGO_FLAGS)
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/release/fastless-rs" python3 tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/release/fastless-rs" FASTLESS_TEST_SIMD=off python3 tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/rust/target/release/fastless-rs" python3 compression_tests.py

compare: fastless rust
	python3 compare_versions.py

clean:
	rm -f fastless literal-tests

.PHONY: check check-rust rust compare clean
