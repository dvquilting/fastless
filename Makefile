CC ?= cc
PKG_CONFIG ?= pkg-config
# Link-time optimization lets the search loops treat the shared document
# globals as unchanged across regexec calls, as when it was one source file.
LTO ?= $(shell $(CC) -flto=auto -x c -c /dev/null -o /dev/null 2>/dev/null && echo -flto=auto || \
	($(CC) -flto -x c -c /dev/null -o /dev/null 2>/dev/null && echo -flto))
CFLAGS ?= -O3 -DNDEBUG $(LTO)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
CMAKE ?= cmake

# gzip input is decoded with Intel ISA-L (libisal) when it is installed: it
# was 7-23% faster than zlib-ng on typical data, though slower on long runs of
# one byte (see README.md). Otherwise the zlib-ng submodule is built as a
# static zlib-compatible library when it is checked out and cmake is
# available, and the installed zlib is the last resort. ZLIB=isal, ng, or
# system selects one explicitly. Run `make clean` after switching.
ZLIB ?= auto
ifeq ($(ZLIB),auto)
ISAL_FOUND := $(shell $(PKG_CONFIG) --exists libisal 2>/dev/null && echo yes || \
	(printf '\043include <isa-l/igzip_lib.h>\nint main(void) { struct inflate_state s; isal_inflate_init(&s); return 0; }\n' | \
	$(CC) -x c - -o /dev/null -lisal 2>/dev/null && echo yes))
override ZLIB := $(if $(ISAL_FOUND),isal,$(if $(and $(wildcard zlib-ng/CMakeLists.txt),$(shell command -v $(CMAKE))),ng,system))
endif
ZLIB_NG := build/zlib-ng
ifeq ($(ZLIB),ng)
ZLIB_LIB := $(ZLIB_NG)/install/lib/libz.a
ZLIB_PC :=
CPPFLAGS += -I$(ZLIB_NG)/install/include
else ifeq ($(ZLIB),system)
ZLIB_LIB :=
ZLIB_PC := zlib
else ifeq ($(ZLIB),isal)
ZLIB_LIB :=
ZLIB_PC :=
CPPFLAGS += -DFASTLESS_ISAL $(shell $(PKG_CONFIG) --cflags libisal 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs libisal 2>/dev/null || echo -lisal)
else
$(error ZLIB must be auto, ng, system, or isal)
endif

# bzip2 input is decoded on every thread with lbzip2's block decoder, built
# from the lbzip2 submodule when it is checked out. BZIP2=system uses the
# installed libbz2 instead, on one thread and about six times slower on
# many cores; BZIP2=lbzip2 requires the submodule. lbzip2 is GPLv3+, so a
# binary built with it is covered by the GPL. Run `make clean` after switching.
BZIP2 ?= auto
ifeq ($(BZIP2),auto)
override BZIP2 := $(if $(wildcard lbzip2/src/decode.c),lbzip2,system)
endif
ifeq ($(BZIP2),lbzip2)
BZIP2_PC :=
BZIP2_OBJECTS := bzip2.o build/lbzip2/decode.o build/lbzip2/parse.o build/lbzip2/crctab.o
CPPFLAGS += -DFASTLESS_LBZIP2
else ifeq ($(BZIP2),system)
BZIP2_PC := bzip2
BZIP2_OBJECTS :=
else
$(error BZIP2 must be auto, lbzip2, or system)
endif
# lbzip2's exported names are generic, so they get a prefix.
LBZIP2_FLAGS := -Ilbzip2/src -Dparse=lbz_parse -Dscan=lbz_scan -Dretrieve=lbz_retrieve \
	-Ddecode=lbz_decode -Demit=lbz_emit -Dparser_init=lbz_parser_init \
	-Ddecoder_init=lbz_decoder_init -Ddecoder_free=lbz_decoder_free \
	-Dcrc_table=lbz_crc_table -Dxmalloc=lbz_xmalloc
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(ZLIB_PC) $(BZIP2_PC) libzstd liblzma 2>/dev/null)
LDLIBS += $(ZLIB_LIB) $(shell $(PKG_CONFIG) --libs $(ZLIB_PC) $(BZIP2_PC) libzstd liblzma 2>/dev/null || \
	echo '$(if $(ZLIB_PC),-lz) $(if $(BZIP2_PC),-lbz2) -lzstd -llzma')

OBJECTS := fastless.o pager.o options.o keys.o render.o input.o literal.o $(BZIP2_OBJECTS)

fastless: $(OBJECTS) search.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

literal-tests: literal_tests.o literal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

# Tiny search and line-count pieces push even the small test files through
# the threaded search and the line-number cache.
# They also decode bzip2 in tiny pieces, missing and misplacing some blocks.
TINY_OBJECTS := $(if $(BZIP2_OBJECTS),$(filter-out bzip2.o,$(OBJECTS)) bzip2-tiny.o,$(OBJECTS))
fastless-tiny: $(TINY_OBJECTS) search-tiny.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

search-tiny.o: search.c
	$(CC) $(CPPFLAGS) $(CFLAGS) '-DSEARCH_NEAR=((size_t)40)' '-DSEARCH_PIECE=((size_t)24)' \
		'-DLINE_PIECE=((size_t)64)' -c -o $@ $<

fastless.o pager.o: pager.h
fastless.o pager.o options.o: options.h input.h
fastless.o pager.o keys.o: keys.h
fastless.o pager.o render.o options.o: render.h
fastless.o pager.o render.o search.o search-tiny.o: search.h literal.h
input.o: input.h bzip2.h $(ZLIB_LIB)

bzip2.o: bzip2.c bzip2.h
	@test -f lbzip2/src/decode.c || { echo 'lbzip2 submodule missing; run: git submodule update --init' >&2; exit 1; }
	$(CC) $(CPPFLAGS) $(LBZIP2_FLAGS) $(CFLAGS) -c -o $@ $<
bzip2-tiny.o: bzip2.c bzip2.h
	$(CC) $(CPPFLAGS) $(LBZIP2_FLAGS) $(CFLAGS) -DBZIP2_SCAN_TEST '-DEMIT_CHUNK=((size_t)4096)' \
		'-DSCAN_STEP=((uint64_t)4096)' '-DBUDGET_PER_THREAD=((size_t)8192)' -c -o $@ $<
# lbzip2's own sources, with its own C dialect and without our warnings.
build/lbzip2/%.o: lbzip2/src/%.c
	@mkdir -p build/lbzip2
	$(CC) $(LBZIP2_FLAGS) $(filter-out -std=% -W%,$(CFLAGS)) -std=gnu11 -c -o $@ $<
literal.o literal_tests.o: literal.h

$(ZLIB_NG)/install/lib/libz.a:
	@test -f zlib-ng/CMakeLists.txt || { echo 'zlib-ng submodule missing; run: git submodule update --init' >&2; exit 1; }
	$(CMAKE) -S zlib-ng -B $(ZLIB_NG) -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$(CC) \
		-DZLIB_COMPAT=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DWITH_GTEST=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_INSTALL_PREFIX="$(CURDIR)/$(ZLIB_NG)/install"
	+$(CMAKE) --build $(ZLIB_NG)
	$(CMAKE) --install $(ZLIB_NG)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
PDFLATEX ?= pdflatex

install: fastless
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	install -m 755 fastless $(DESTDIR)$(BINDIR)/fastless
	install -m 644 doc/fastless.1 $(DESTDIR)$(MANDIR)/man1/fastless.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/fastless $(DESTDIR)$(MANDIR)/man1/fastless.1

# The PDF manual. pdflatex runs twice so that the contents have page numbers.
doc: doc/fastless.pdf

doc/fastless.pdf: doc/manual.tex
	@mkdir -p build/doc
	@for pass in 1 2; do \
		$(PDFLATEX) -interaction=nonstopmode -halt-on-error -output-directory=build/doc $< >/dev/null || \
			{ tail -n 20 build/doc/manual.log; exit 1; }; \
	done
	cp build/doc/manual.pdf $@

check: fastless fastless-tiny literal-tests
	python3 doc_tests.py
	./literal-tests
	python3 tests.py
	FASTLESS_TEST_SIMD=off python3 tests.py
	python3 compression_tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/fastless-tiny" FASTLESS_TEST_ONLY=bzip2 python3 compression_tests.py
	python3 pager_tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/fastless-tiny" python3 tests.py
	FASTLESS_TEST_EXE="$(CURDIR)/fastless-tiny" python3 pager_tests.py

clean:
	rm -f fastless fastless-tiny literal-tests *.o
	rm -rf build

.PHONY: check clean install uninstall doc
