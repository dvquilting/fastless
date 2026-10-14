CC ?= cc
PKG_CONFIG ?= pkg-config
CARGO ?= cargo
CARGO_FLAGS ?= --locked
CFLAGS ?= -O3 -DNDEBUG
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
CMAKE ?= cmake

# gzip input uses the zlib-ng submodule, built as a static zlib-compatible
# library, when it is checked out and cmake is available. ZLIB=system uses the
# installed zlib instead; ZLIB=ng requires the submodule. Run `make clean` after
# switching.
ZLIB ?= auto
ifeq ($(ZLIB),auto)
override ZLIB := $(if $(and $(wildcard zlib-ng/CMakeLists.txt),$(shell command -v $(CMAKE))),ng,system)
endif
ZLIB_NG := build/zlib-ng
ifeq ($(ZLIB),ng)
ZLIB_LIB := $(ZLIB_NG)/install/lib/libz.a
ZLIB_PC :=
CPPFLAGS += -I$(ZLIB_NG)/install/include
else ifeq ($(ZLIB),system)
ZLIB_LIB :=
ZLIB_PC := zlib
else
$(error ZLIB must be auto, ng, or system)
endif
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(ZLIB_PC) libzstd liblzma 2>/dev/null)
LDLIBS += $(ZLIB_LIB) $(shell $(PKG_CONFIG) --libs $(ZLIB_PC) libzstd liblzma 2>/dev/null || echo '$(if $(ZLIB_PC),-lz) -lzstd -llzma')

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
input.o: input.h $(ZLIB_LIB)
literal.o literal_tests.o: literal.h

$(ZLIB_NG)/install/lib/libz.a:
	@test -f zlib-ng/CMakeLists.txt || { echo 'zlib-ng submodule missing; run: git submodule update --init' >&2; exit 1; }
	$(CMAKE) -S zlib-ng -B $(ZLIB_NG) -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$(CC) \
		-DZLIB_COMPAT=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DWITH_GTEST=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_INSTALL_PREFIX="$(CURDIR)/$(ZLIB_NG)/install"
	+$(CMAKE) --build $(ZLIB_NG)
	$(CMAKE) --install $(ZLIB_NG)

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
	rm -rf build

.PHONY: check check-rust rust compare clean
