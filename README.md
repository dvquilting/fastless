# fastless

A 64-bit C pager with less's keys and options, designed for searching 1–200 GB
files, including gzip, zstd, xz, and bzip2 input. It maps plain files read-only;
compressed input, pipes, and standard input are read into memory, or into a
temporary file once their size exceeds `--max-mem` (10 GiB by default), and
then accessed read-only. Pipes are paged while they are still being written.
It never constructs a line index or copies a plain file into RAM. Linux and macOS are supported targets.

```sh
make
./fastless huge.log
./fastless --literal huge.log
./fastless --glob huge.log
LESS='-iR' ./fastless huge.log other.log.zst
some-command | ./fastless
```

The manual page, [doc/fastless.1](doc/fastless.1) (`man ./doc/fastless.1`), is
the reference for every command and option, and
[doc/fastless.pdf](doc/fastless.pdf) is a typeset user manual; both list the
differences from GNU less. `make install` installs the program and the manual
page under `PREFIX` (default `/usr/local`), and `make doc` rebuilds the PDF from
`doc/manual.tex` with pdflatex.

Builds with GCC or Clang and links libzstd, liblzma, and gzip and bzip2 decoders.
`pkg-config` provides their compiler/linker flags, with standard library names as a fallback.
For gzip input, the build uses [Intel ISA-L](https://github.com/intel/isa-l)
when it is installed (Debian and Ubuntu: `libisal-dev`; Fedora: `isa-l-devel`;
Homebrew: `isa-l`), found with `pkg-config` or a test compile. Otherwise it
uses [zlib-ng](https://github.com/zlib-ng/zlib-ng), pinned as a git submodule
and built with cmake into a static zlib-compatible library; it decoded gzip
about 1.7x faster than zlib 1.3.1 in local tests. Clone with
`git clone --recursive`, or run `git submodule update --init` in an existing
checkout. Without either, the build falls back to the installed zlib.
`make ZLIB=isal`, `make ZLIB=ng`, or `make ZLIB=system` selects a decoder
explicitly. ISA-L is linked as a shared library, so a binary built with it
needs `libisal` installed where it runs; `make ZLIB=ng` builds one that
doesn't. Run `make clean` after switching.

bzip2 input is decoded with the block decoder from
[lbzip2](https://github.com/kjn/lbzip2), pinned as a second git submodule and
compiled in (its `decode.c`, `parse.c`, and `crctab.c`). Like lbzip2, fastless
decodes bzip2's independent blocks on every thread, so it is several times
faster than libbz2 on a multicore machine. lbzip2 is licensed under the GPL,
version 3 or later, which is compatible with fastless's GPLv3. Without the
submodule the build uses the installed libbz2, on one thread;
`make BZIP2=system` selects it explicitly and `make BZIP2=lbzip2` makes lbzip2
mandatory.

bzip2 decoders on the same machine, decoding 556 MiB of varied logs that
`bzip2 -9` compressed 6.7x (wall time, best of 3; fastless includes a search):

| Threads | libbz2 1.0.8 (`bzip2 -d`) | lbzip2 2.5 (`lbzip2 -d -n`) | fastless |
| ---: | ---: | ---: | ---: |
| 1 | 11.6 s (48 MiB/s) | 9.5 s (58 MiB/s) | 9.4 s (59 MiB/s) |
| 4 | | 2.4 s (229 MiB/s) | 2.4 s (230 MiB/s) |
| 16 | | 1.8 s (314 MiB/s) | 1.8 s (306 MiB/s) |

Workers scan ahead for the 48-bit pattern that begins every block and decode
each block found. The main thread follows the stream with lbzip2's parser,
which checks every header and the stream CRC, and uses a worker's block only
when it starts exactly where the previous block ended and its CRC matches. The
pattern can also occur inside compressed data; those finds are decoded and
discarded. A block the scan misses is decoded by the main thread. Blocks of
one repeated byte can expand to 45 MB each, so decoded blocks waiting to be
used are limited to 16 MiB per thread, except the next one needed. On 2 GiB of
zeros that took 1.4 s on 16 threads, against lbzip2's 1.3 s.

gzip decoders compared on a Ryzen 7 5700X, one thread, decoded MiB/s (decoding
alone, best of 5, ~1 GiB of output each; GNU gzip 1.14 at level 6 unless noted):

| Data (compression ratio) | zlib 1.3.1 | zlib-ng 2.3.3 | ISA-L 2.31.1 | libdeflate 1.25 |
| --- | ---: | ---: | ---: | ---: |
| Varied logs, level 1 (2.5x) | 412 | 802 | 931 | 1005 |
| Varied logs (3.1x) | 435 | 802 | 1058 | 1040 |
| Varied logs, level 9 (3.1x) | 439 | 806 | 1062 | 1036 |
| Shared libraries (2.8x) | 416 | 788 | 892 | 1001 |
| C headers and docs (7x) | 817 | 1951 | 2115 | 2460 |
| JSON service logs (12x) | | 2910 | 3426 | 3714 |
| Syslog templates (8x) | | 1920 | 2596 | 2458 |
| Repeated stack traces (92x) | | 10397 | 9526 | 8908 |
| Long runs of one byte (370x) | 759 | 12681 | 4235 | 8380 |
| Zeros (1000x) | | 12924 | 3356 | 9689 |

ISA-L 2.32.1 measured within 2% of 2.31.1. In fastless itself (decode plus
search), `ZLIB=isal` took 7-23% less time than zlib-ng on the first seven kinds
of data and 2-2.5 times as long on the last two (0.4 s instead of 0.18 s per
GiB of zeros), which is why it is the default: typical logs and archives
decode faster, and the slow case is data that decodes quickly anyway.
libdeflate is fast on everything but decodes only whole buffers: each gzip
member needs an output buffer of its full, unknown size (the trailer holds it
modulo 4 GiB) in one uninterruptible call with no progress, which does not fit
decoding 200 GB files in bounded steps with Ctrl-C and `--max-mem` spilling.
Requires a 64-bit process and POSIX `REG_STARTEND` support (Linux and macOS).
Each source file compiles separately, so `make -j` builds in parallel.

```sh
# Debian / Ubuntu build dependencies
sudo apt install build-essential pkg-config cmake libisal-dev zlib1g-dev libbz2-dev libzstd-dev liblzma-dev

# macOS with Homebrew
brew install pkgconf cmake isa-l zlib bzip2 zstd xz
PKG_CONFIG_PATH="$(brew --prefix zlib)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" make PKG_CONFIG=pkgconf
```

Homebrew formula details: [pkgconf](https://formulae.brew.sh/formula/pkgconf),
[isa-l](https://formulae.brew.sh/formula/isa-l),
[zlib](https://formulae.brew.sh/formula/zlib),
[zstd](https://formulae.brew.sh/formula/zstd),
[xz](https://formulae.brew.sh/formula/xz).

`make check` (or `make -j check`) runs the Python 3 integration suites, including a sparse 200 GiB
file, real pseudo-terminal output, compressed streams, and cancellation cleanup.
`pager_tests.py` drives the interactive commands and options through a small
terminal emulator and checks the rendered screen.
It also compares AVX2 and portable literal kernels against a bytewise reference,
including inaccessible page boundaries, all byte values, overlapping matches,
and both search directions. Pager integration runs with SIMD auto and off,
and again with a build using tiny search and line-count pieces so that the
small test files exercise the threaded search and the line-number cache. The
same build decodes bzip2 in 4 KiB pieces with a scan that misses every third
block and finds a false start before the others, which exercises the main
thread's decoding and the discarding of wasted work.
Python 3 tests use its gzip/lzma/bz2 modules and libzstd via ctypes (libzstd 1.4+).
The optional threaded-xz fixture also uses the `xz` executable if installed.
Linux has been tested locally; macOS still needs a native build/test run.

## Compressed input

```sh
./fastless huge.log.gz
./fastless huge.log.zst
./fastless huge.log.xz
./fastless huge.log.bz2
./fastless --literal --find 'needle' huge.log.zst
./fastless --max-mem 2 huge.log.gz
./fastless --max-mem 0 --tmpdir /path/to/fast/disk huge.log.xz
./fastless --threads 4 huge.log.xz
```

Format detection uses magic bytes, so filenames/extensions do not matter.
`--compression auto|none|gzip|zstd|xz|bzip2` overrides detection; `none` opens
bytes exactly as stored. gzip members, zstd frames (including skippable
frames), xz streams, and bzip2 streams (as pbzip2 writes them) may be
concatenated. Only zero bytes may follow the last one; bzip2 itself ignores
any trailing data, but fastless reports it, as for the other formats. Streams are fully decoded and their supported
integrity checks verified before paging or returning search results. Corrupt,
truncated, or unsupported streams produce an error rather than partial results.
External zstd dictionaries are not supported.

Decoded output is kept in anonymous memory until it exceeds `--max-mem GiB`,
a non-negative decimal number of GiB (2^30 bytes) such as `10`, `0.5`, or `.25`;
the default is 10. Output of exactly that size stays in memory. Larger output
moves to a temporary file: what was decoded so far is written out, its memory is
freed, and decoding continues into the file. `--max-mem 0` writes all output to a
file. Only address space is reserved up front; memory is committed as output
grows, using transparent huge pages where Linux allows them. Decoded data held
in memory needs that much RAM (or swap) for as long as the pager runs, so lower
`--max-mem` on machines with less free memory than the limit. Compressed input
read from a pipe is held in memory too, and counts against the same limit, so
its decoded output gets only what is left. On Linux with
strict overcommit, output moves to a file early if memory cannot be committed.

Temporary files are created with mode 0600 in `--tmpdir DIR`, otherwise `$TMPDIR`
or `/tmp`, and unlinked immediately; the directory is only used once output
exceeds `--max-mem`. The mapping keeps their storage alive until
the pager exits; exiting, cancelling, or killing the process releases it. There
is no persistent decompression cache. You need free space for the **full decoded
size**, which may be much larger than the compressed file. `/tmp` may be a
memory-backed filesystem on Linux; choose a disk-backed directory for large
files. The program does not fsync temporary data or modify the source file.

## Pipes, standard input, and preprocessors

Without a file argument, or with `-`, standard input is read; keys then come
from `/dev/tty`. Named pipes are read without `-f`; devices and other
non-regular files need `-f`, and directories are refused.

These streams are paged while they are still being written, as in less, so
`make 2>&1 | fastless` and `tail -f log | fastless` show output as it arrives. A
background thread reads the stream into one fixed address range, into memory
up to `--max-mem` and then a temporary file mapped over the same addresses.
The pager only sees the stream grow, between commands, so searching and
drawing work exactly as they do for files, at the same speed. While a stream
is being read, its size and percentages are unknown, as in less: the status
shows `(reading)`, and prompts' `?s`, `?B`, `?p`, `?P`, `?L`, `?D`, and `?e`
are false.

- Moving forward past the end waits for more input ("Waiting for data...").
- `G`, `%`, and `'$` follow the end until the input ends; `F` follows it for
  good.
- Forward searches go on into input as it arrives. The last, unfinished line
  is searched only once it is complete.
- Line, byte, and percent jumps past what has arrived wait for it.

Any key ends the wait and runs as the next command, and Ctrl-C ends it
without running anything.

Input that fits in `--max-mem` (but at least 64 MiB) is read ahead in the
background, which keeps the writer from blocking. Beyond that, input is read
only as far as the screen, a search, or a jump needs, which is how less reads
all input. So `yes | fastless` does not fill the temporary directory. A compressed
stream is read whole, then decoded, and appears when decoding ends; its status
shows `(reading compressed input)` and then `(decompressing)`. LESSOPEN output
is still read to its end first, because whether it was empty decides which
file is shown.

`LESSOPEN` and `LESSCLOSE` work as in less: `|cmd %s` reads the command's output
(the original file is used if it is empty), `||cmd %s` treats empty output with
a zero status as an empty file, and `cmd %s` prints a replacement file name that
`LESSCLOSE` is later given with the original. `-L` ignores `LESSOPEN`, and
`--show-preproc-errors` reports a preprocessor's failure. Files with gzip,
zstd, xz, or bzip2 magic skip the preprocessor, because the native threaded decoders
are faster than a pipe from an external decompressor; `--compression none` with
`-L` views their raw bytes. Preprocessors do not apply to standard input.

When standard output is not a terminal, the input is copied to it unpaged, as
less does, which makes `fastless file.zst | grep ...` work. A pipe is copied as
it arrives and not kept, so `tail -f log | fastless | grep ...` runs in constant
memory; on Linux the kernel moves the data with `splice`. A compressed pipe is
still read whole and decoded first. `--find` reads a pipe to its end before
searching it, showing progress on a terminal's stderr.

Decoding a file finishes before the first page or search is available,
including when `--offset` is specified. Offsets, navigation, and the status bar refer to decoded
bytes. Terminal stderr shows decoding progress; Ctrl-C cancels it. No pipes or
external decoder processes are used by fastless itself.

Searches reaching more than 8 MiB from their starting point are divided among
threads in 4 MiB pieces; the first match in file order is still the one
reported. `--find --all` also searches the whole range on every thread and
prints the matches in order. A regex or glob is first reduced to the longest literal every match
must contain, and only lines holding it are matched in full.

The native C decoders write directly into the in-memory output, with compressed
input accessed through mmap. Once output moves to a temporary file, they fill
8 MiB buffers that a writer thread appends to it instead. Searching and
xz decoding automatically use up to 32 logical CPUs; `--threads N` selects
1–256, or 0 for automatic.
liblzma 5.4+ supports the threaded decoder; older versions use a single thread.
Parallel decoding requires xz blocks with size metadata, usually produced by a
threaded encoder; single-block files stay single-threaded. See the
[liblzma decoder documentation](https://tukaani.org/xz/liblzma-api/container_8h.html).
gzip and standard zstd streams are decoded with one decoder thread. xz limits
thread buffering to a sixteenth of RAM (from 256 MiB to 2 GiB) and decoder
memory to 512 MiB, reducing threads when necessary. zstd uses its library's default maximum window (128 MiB). These limits
are independent of decoded file size; unusually large dictionaries/windows may
be rejected. OS page caching and mapped pages are additional memory use.

## Searching

Regular expressions are the default: POSIX extended regex, matched within
individual lines, without their trailing newline. `/` enters a forward search,
`?` a backward search. `n` repeats the original direction; `N` reverses it, and
`ESC-n`/`ESC-N` continue into the following or preceding files. A count finds
the N-th match: `3/error` or `3n`. Search results are reported by byte offset
and positioned at the jump target line (`-j`, the top line by default). A new
forward search starts at the target line, `-a` starts it after the screen and
`-A` after the target line. All visible matches of the current pattern are
highlighted in reverse video, including overlapping matches; `-g` highlights
only the match found, `-G` nothing, `ESC-u` toggles highlighting, and `ESC-U`
forgets the pattern. Regex and literal modes highlight matching bytes; glob mode
highlights the whole matching line. A match outside the visible columns shifts
the view, keeping `--match-shift` columns to its left.

A pattern may begin with less's modifiers, which change the prompt's label:

| Modifier | Effect |
| --- | --- |
| Ctrl-N or `!` | Find lines that do **not** match |
| Ctrl-E or `*` | Continue through the following (or preceding) files |
| Ctrl-F or `@` | Start at the first file (`/`) or the last file (`?`) |
| Ctrl-K | Set and highlight the pattern without moving |
| Ctrl-R | Search literally (fastless's literal mode) for this pattern |
| Ctrl-S *n* | Match regex group *n*; highlight just that group |
| Ctrl-W | Wrap to the other end of the file if nothing is found |
| Ctrl-L | Enter the next character literally |

The punctuation forms `!`, `*`, and `@` are modifiers only in regex mode: glob
patterns such as `*error*` and literal ones such as `!important` start with
them. `--search-options=EFKNRW` sets modifiers for every search. `n` and `N`
keep the modifiers except Ctrl-F and Ctrl-K, as in less. An empty pattern
reuses the current one with the new modifiers.

Switch modes with `ESC-r` (regex), `ESC-l` (literal), or `ESC-w` (glob), or
with the `--regex`, `--literal`, and `--glob` options at the `--` prompt.
Switching modes recompiles the current pattern; an invalid regex leaves the
old mode active. (less uses `r` to repaint and `w` to move back a window, so
fastless's former single-letter mode keys now do that.)

`&pattern` displays only lines that match (or with Ctrl-N, lines that do not);
`&` alone shows every line again. Filtering keeps no index: each line shown is
found with the same threaded search, so a page of sparse matches costs a scan
of the bytes between them. Movement, searches, and marks then refer to the
lines shown. `--incsearch` searches as the pattern is typed; each keystroke
cancels the search still running.

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
  the fast matcher, and the regex runs only on lines containing it. For
  top-level alternatives such as `timeout|refused` (up to eight), each
  alternative's run is looked for, and lines holding any of them are matched.
  Characters followed by `*`, `+`, `?`, or `{` are not part of a run; patterns
  without a usable run use the full regex path.

`-i` ignores case only when the pattern contains no ASCII uppercase letters
(less's smart-case behavior). `-I` always ignores case; `--case-sensitive`
turns both off. Matching uses the C locale: case folding is ASCII, not
Unicode. Files and patterns are bytes, so a UTF-8 pattern matches its bytes.

## Navigation

The keys are less's (`h` shows the full summary inside the pager). Commands
marked * take a count typed first, such as `50%` or `10j`.

| Key | Action |
| --- | --- |
| Space `f` Ctrl-F Ctrl-V Page Down * | Forward a window (or N lines) |
| `b` Ctrl-B ESC-`v` Page Up * | Back a window (or N lines) |
| `z` / `w` * | Forward / back a window, setting the window to N |
| ESC-Space * | Forward a window even at the end of the file |
| `j` `e` Enter Ctrl-E Ctrl-N Down * | Forward a line (or N) |
| `k` `y` Ctrl-Y Ctrl-P Ctrl-K Up * | Back a line (or N) |
| `J` / `K` `Y` * | Forward / back a line, even at the end |
| `d` Ctrl-D / `u` Ctrl-U * | Forward / back half a window, setting it to N |
| ESC-`)` Right / ESC-`(` Left * | Shift right / left half a screen (or N columns) |
| ESC-`}` Ctrl-Right / ESC-`{` Ctrl-Left | Show the end of the longest line / column 1 |
| `g` `<` ESC-`<` Home * | First line (or line N) |
| `G` `>` ESC-`>` End * | Last line (or line N) |
| `p` `%` * | N percent into the file (decimals allowed) |
| `P` * | Byte offset N (fastless) |
| `{` `(` `[` / `}` `)` `]` * | Matching close / open bracket of the top / bottom line |
| ESC-Ctrl-F *ab* / ESC-Ctrl-B *ab* | The same for any bracket pair *a b* |
| `m`*x* / `M`*x* | Mark the top / bottom line with letter *x* |
| `'`*x* Ctrl-X Ctrl-X | Go to mark *x*; `''` the previous position, `'^` `'$` the ends |
| ESC-`m`*x* | Clear mark *x* |
| `F` / ESC-`F` | Follow a growing file / until the pattern appears |
| `r` Ctrl-R Ctrl-L / `R` | Repaint / reread a changed file and repaint |
| `/` `?` `n` `N` ESC-`n` ESC-`N` `&` | Searching and filtering (above) |
| ESC-`u` / ESC-`U` | Toggle / clear highlighting |
| ESC-`r` ESC-`l` ESC-`w` | Regex / literal / glob mode (fastless) |
| `:e` Ctrl-X Ctrl-V / `:n` `:p` `:x` * / `:d` | Examine a file / next, previous, N-th / drop from list |
| `=` Ctrl-G `:f` | File name and position |
| `-`*x* `--`*name* / `_`*x* `__`*name* | Change / show an option; `-+x` resets it |
| `+`*cmd* | Run *cmd* in each file examined from now on |
| `!`*cmd* `#`*cmd* | Shell command (`%` file, `#` previous file, `!!` last command); `#` expands prompt escapes |
| `|`*x* *cmd* | Pipe from mark *x* (or `^`, `$`, `.`) through the screen to *cmd* |
| `s` *file* | Save the input (for example, decoded compressed data) |
| `v` | Edit the file with `$VISUAL`, `$EDITOR`, or `vi` (`LESSEDIT` is honoured) |
| `h` `H` / `V` | Help / version |
| `q` `Q` `:q` `:Q` `ZZ` | Quit (or leave the help) |
| Ctrl-C | Cancel a search, count, or follow |
| Ctrl-Z | Suspend |

With `--mouse`, the wheel scrolls `--wheel-lines` lines (`--MOUSE` reverses
it) and a click sets mark `#`.

### Line numbers

`-N` shows a line-number column (`--line-num-width`, default 7, widening when
the numbers on the screen need more digits; `--no-number-headers` leaves header
lines unnumbered). Prompts can show line numbers too: `-M` and `=` give
`lines 6-12/100`, as in less, and `-P` prompts may use `%l`, `%L`, `%d`, `%D`,
`%P` and their `?` conditionals. `-n` turns all of these off.

Nothing is counted unless something on the screen shows a line number, so
without `-N` or a line-number prompt there is no cost at all. When numbers are
needed, newlines are counted with SIMD on all search threads and cached per
64 MiB piece, with each file keeping its counts while another is current. A
line near one already numbered (scrolling, nearby searches) costs only the
bytes in between, and a position whose preceding pieces are counted costs at
most one 64 MiB scan. A long jump into uncounted territory never waits: the
screen appears at once with the numbers left blank, the pieces are counted a
few at a time while no key is waiting, and the numbers appear when known. On
an 8-core desktop with the file cached, numbering the end of a 4 GiB,
84-million-line file took 0.15 s; a cold file is counted at storage speed. A
counter in the status line shows progress for longer counts, and Ctrl-C during
one turns line numbers off, as in less. `=`, `v` (which passes `+line` to the
editor, as less's default `LESSEDIT` does), and `Ng` are explicit requests and
count before answering, interruptibly.

Prompts have less's line editing: Left/Right and ESC-`h`/ESC-`l`, word moves
(Ctrl-Left/Right, ESC-`b`/ESC-`w`), Home/End (ESC-`0`/ESC-`$`), Delete
(ESC-`x`), word deletion (ESC-Backspace, ESC-`X`), Ctrl-U to clear, Ctrl-V to
enter a character literally, and Up/Down for history, matching what was typed.
Backspace on an empty prompt, Ctrl-G, or a lone Escape cancels. Search and shell
history is kept in `$FASTLESSHISTFILE` (default `~/.fastlesshst`, `-` for none),
separate from less's file; `--save-marks` keeps marks there too.

### Display

Long lines are chopped, with `--rscroll`'s character (default `>`, `-` for
none) in the last column; there is no wrapping. Text is shown as UTF-8 when
`LESSCHARSET` or the locale (`LC_ALL`, `LC_CTYPE`, `LANG`) says UTF-8, using the
C library's character widths; otherwise bytes above 127 show as `<XX>`. Control
characters show in bold as `^X`, invalid or unprintable characters as `<XX>`
or `<U+XXXX>`, so file contents cannot send escape sequences to the terminal;
`-R` (and `-r`, which fastless treats the same) passes ANSI color sequences
through, as less does. Following less, `x` Backspace `x` is bold, `_`
Backspace `x` is underlined, a carriage return before a newline is hidden, and
tabs expand to `-x` stops (default 8). `-u`, `-U`, and `--proc-backspace`,
`--proc-return`, `--proc-tab` (with uppercase forms) choose whether those
bytes are processed or shown as control characters; unlike less, `-u` never
sends raw backspaces or carriage returns to the terminal.

`-s` squeezes runs of blank lines, `-~` omits the tildes after the end,
`-J` adds a status column showing marks and lines with matches, `--status-line`
highlights marked lines, `-w`/`-W` highlight the first new line after moving
forward, and `--header=L,C,N` keeps L lines (starting at line N) at the top and
C columns at the left (`--no-search-header-lines`, `--no-search-header-columns`
exclude them from searches).

The default status line is fastless's own; `-m`, `-M`, and `-P` select less's
prompts and prompt language (`-Ps`, `-Pm`, `-PM`, `-P=`, `-Ph`, `-Pw`), with
`%b`, `%B`, `%c`, `%E`, `%f`, `%F`, `%g`, `%i`, `%m`, `%p`, `%s`, `%t`, `%T`,
`%x`, the line items above, and the `?` conditionals. A line item shows `?`,
and its conditional is false, until the number is known.

The pager uses the alternate screen and restores terminal settings on exit;
`-X` draws on the normal screen instead, and `--redraw-on-quit` leaves the
final screen on the terminal. `-c` repaints by clearing. `-F` prints a file
that fits on one screen and exits; `-e` quits on a forward command at the end of
the file and `-E` as soon as the end is shown. `-K` quits on Ctrl-C with status
2, and `-q`/`-Q` quiet the bell for movement past either end / for everything.
`-z` sets the window size (negative counts from the screen height), `-j` the
target line (`.5` is the middle, negative counts from the bottom), and `-#`
the horizontal step.

### Options, $LESS, and key bindings

Options are read from `LESS` first, as in less (string values end at `$`;
unknown options there are ignored), then from the command line. Long options
may be abbreviated to a unique prefix. `+cmd` runs a command for the first
file and `++cmd` for every file, so `+G`, `+/pattern`, `+50%`, and `-p pattern`
work. Options with no effect in fastless are accepted silently: `-b`, `-B`,
`-d`, `-h`, `-S` (lines are always chopped), `-y`, `--file-size`,
`--no-keypad`, `--no-vbell`, and `--exit-follow-on-close`. These are rejected
with a message: `-o`/`-O` (log files), `-t`/`-T` (tags), `-D`/`--use-color`,
`--wordwrap`, `--modelines`, `-"`, `--use-backslash`, and `-k` compiled lesskey
files.

Key bindings can be changed with lesskey source files: `--lesskey-src=FILE`,
or else the first of `$LESSKEYIN`, `$XDG_CONFIG_HOME/lesskey`,
`~/.config/lesskey`, and `~/.lesskey`, plus `$LESSKEY_CONTENT` and
`--lesskey-content=TEXT` (lines separated by newlines or `;`). The
`#command`, `#line-edit`, and `#env` sections, `#stop`, `\k` key names, and
extra strings work as in lesskey(1); actions fastless lacks (tags, OSC 8 links)
are accepted but do nothing. `LESSSECURE=1` disables shell commands, pipes,
saving, editing, `:e`, preprocessors, and history writing.

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

Offsets are zero-based and refer to the decoded file; `LESSOPEN` is not used. `--all` includes overlapping and zero-length regex
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
searches on the calling thread.

Keep the mapped file stable while viewing it. In-place truncation can cause
`SIGBUS`, as with other mmap readers. Replacing its pathname does not update
the existing mapping unless `F` is used with `--follow-name`; `F` remaps a
regular file as it grows. Only the current file stays open: switching files
releases a decoded file's memory, and returning to it decodes it again.
Standard input, pipes, and devices cannot be read again, so they stay open and
keep what was read. Compressed source files must also stay stable during
decoding. Only compression is removed; archive entries such as tar members
are not extracted.

`python3 benchmark.py --mib 256` measures first-match throughput on a generated
file. This is a warm-cache microbenchmark, not a storage-speed prediction.

`python3 compression_benchmark.py --mib 256 --runs 3` measures end-to-end
compressed-file opening plus literal search, compared with the corresponding
CLI decoder writing a file followed by fastless. Fixture generation is excluded;
requires the gzip, zstd, and xz executables. `--tmpdir DIR` selects the benchmark's
storage. Results depend strongly on data, compression settings, and storage.

## License

fastless is licensed under the GNU General Public License, version 3; see
[LICENSE](LICENSE).
