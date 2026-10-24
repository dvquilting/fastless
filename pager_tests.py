#!/usr/bin/env python3
"""Interactive pager tests: a small terminal emulator renders the program's
output, and each check waits for the expected screen."""
import codecs
import fcntl
import gzip
import os
import pathlib
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unicodedata

EXE = os.environ.get('FASTLESS_TEST_EXE', str(pathlib.Path(__file__).with_name('fastless').resolve()))
checks = 0


class Screen:
    """Enough of a VT100 for fastless: cursor addressing, line and screen
    clears, SGR attributes, the alternate screen, and UTF-8 text."""
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.bells = 0
        self.raw = b''
        self.pending = b''
        self.alt = False
        self.saved = None
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.clear()
        self.r = self.c = 0
        self.attrs = set()

    def clear(self):
        self.grid = [[' '] * self.cols for _ in range(self.rows)]
        self.style = [[frozenset()] * self.cols for _ in range(self.rows)]

    def newline(self):
        self.r += 1
        if self.r >= self.rows:
            self.grid.pop(0)
            self.style.pop(0)
            self.grid.append([' '] * self.cols)
            self.style.append([frozenset()] * self.cols)
            self.r = self.rows - 1

    def put(self, ch):
        cat = unicodedata.category(ch)
        width = 0 if cat in ('Mn', 'Me') else 2 if unicodedata.east_asian_width(ch) in 'WF' else 1
        if width == 0:
            if self.c:
                self.grid[self.r][self.c - 1] += ch
            return
        if self.c + width > self.cols:
            self.c = 0
            self.newline()
        self.grid[self.r][self.c] = ch
        self.style[self.r][self.c] = frozenset(self.attrs)
        if width == 2 and self.c + 1 < self.cols:
            self.grid[self.r][self.c + 1] = ''
            self.style[self.r][self.c + 1] = frozenset(self.attrs)
        self.c += width

    def csi(self, private, params, final):
        nums = [int(x) if x.isdigit() else 0 for x in params.split(';')] if params else []
        if final in 'Hf':
            self.r = max(1, nums[0] if nums else 1) - 1
            self.c = max(1, nums[1] if len(nums) > 1 else 1) - 1
            self.r = min(self.r, self.rows - 1)
            self.c = min(self.c, self.cols - 1)
        elif final == 'K':
            mode = nums[0] if nums else 0
            lo, hi = (self.c, self.cols) if mode == 0 else (0, self.c + 1) if mode == 1 else (0, self.cols)
            for j in range(lo, hi):
                self.grid[self.r][j] = ' '
                self.style[self.r][j] = frozenset()
        elif final == 'J':
            if (nums[0] if nums else 0) == 2:
                self.clear()
        elif final == 'm':
            for n in nums or [0]:
                if n == 0:
                    self.attrs.clear()
                elif n == 1:
                    self.attrs.add('bold')
                elif n == 4:
                    self.attrs.add('under')
                elif n == 7:
                    self.attrs.add('rev')
                elif 30 <= n <= 37:
                    self.attrs.add('color')
        elif final in 'hl' and private == '?':
            if '1049' in params.split(';'):
                if final == 'h' and not self.alt:
                    self.saved = (self.grid, self.style, self.r, self.c)
                    self.clear()
                    self.alt = True
                elif final == 'l' and self.alt:
                    self.grid, self.style, self.r, self.c = self.saved
                    self.alt = False

    def feed(self, data):
        self.raw += data
        buf = self.pending + data
        i = 0
        text = b''
        while i < len(buf):
            b = buf[i]
            if b == 0x1b:
                if text:
                    for ch in self.decoder.decode(text):
                        self.put(ch)
                    text = b''
                if i + 1 >= len(buf):
                    break
                if buf[i + 1] == ord('['):
                    j = i + 2
                    while j < len(buf) and not 0x40 <= buf[j] <= 0x7e:
                        j += 1
                    if j >= len(buf):
                        break
                    body = buf[i + 2:j].decode('latin1')
                    private = body[:1] if body[:1] in '?<>=' and body else ''
                    self.csi(private, body[len(private):], chr(buf[j]))
                    i = j + 1
                else:
                    i += 2
                continue
            if b < 0x20 or b == 0x7f:
                if text:
                    for ch in self.decoder.decode(text):
                        self.put(ch)
                    text = b''
                if b == 0x0d:
                    self.c = 0
                elif b == 0x0a:
                    self.newline()
                elif b == 0x07:
                    self.bells += 1
                elif b == 0x08:
                    self.c = max(0, self.c - 1)
                i += 1
                continue
            text += bytes([b])
            i += 1
        if text:
            for ch in self.decoder.decode(text):
                self.put(ch)
        self.pending = buf[i:]

    def line(self, i):
        return ''.join(self.grid[i]).rstrip()

    def lines(self):
        return [self.line(i) for i in range(self.rows)]


class Pager:
    def __init__(self, args, rows=8, cols=100, env=None, stdin=None, cwd=None, setsid=False):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        self.original = termios.tcgetattr(slave)
        e = dict(os.environ, LESS='', LESSOPEN='', LESSCLOSE='', LANG='C.UTF-8', LC_ALL='',
                 LESSCHARSET='', FASTLESSHISTFILE='-', LESSKEYIN='/nonexistent', HOME='/nonexistent')
        e.pop('LESSSECURE', None)
        if env:
            e.update(env)

        def controlling():
            os.setsid()
            fcntl.ioctl(1, termios.TIOCSCTTY, 0)
        self.child = subprocess.Popen([EXE, *args], stdin=stdin if stdin is not None else slave,
                                      stdout=slave, stderr=slave, env=e, cwd=cwd,
                                      preexec_fn=controlling if setsid else None)
        self.slave = slave
        self.screen = Screen(rows, cols)

    def pump(self, seconds=0.0):
        end = time.monotonic() + seconds
        while True:
            wait = max(0, end - time.monotonic())
            if not select.select([self.master], [], [], wait if wait else 0.02)[0]:
                if time.monotonic() >= end:
                    return
                continue
            try:
                self.screen.feed(os.read(self.master, 65536))
            except OSError:
                return

    def wait(self, predicate, what='', timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump(0.03)
            if predicate(self):
                return
        raise AssertionError(f'Timed out waiting for {what}:\n' + '\n'.join(
            f'{i:2}|{line}' for i, line in enumerate(self.screen.lines())) + f'\nraw tail: {self.screen.raw[-600:]!r}')

    def keys(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master, data)

    def lines(self):
        return self.screen.lines()

    def status(self):
        return self.screen.line(self.screen.rows - 1)

    def top_byte(self):
        m = re.search(r'byte (\d+)/', self.status())
        return int(m.group(1)) if m else None

    def expect_top(self, keys, byte, what=''):
        global checks
        self.keys(keys)
        self.wait(lambda p: p.top_byte() == byte and 'Searching' not in p.status(),
                  what or f'top byte {byte} after {keys!r}')
        checks += 1

    def expect_status(self, keys, text):
        global checks
        if keys:
            self.keys(keys)
        self.wait(lambda p: text in p.status(), f'status containing {text!r} after {keys!r}')
        checks += 1

    def expect_row(self, keys, row, text, exact=True):
        global checks
        if keys:
            self.keys(keys)
        self.wait(lambda p: (p.screen.line(row) == text) if exact else (text in p.screen.line(row)),
                  f'row {row} == {text!r} after {keys!r}')
        checks += 1

    def quit(self, code=0, key=b'q'):
        global checks
        if self.child.poll() is None and key:
            self.keys(key)
        try:
            rc = self.child.wait(timeout=5)
        finally:
            if self.child.poll() is None:
                self.child.kill()
                self.child.wait()
        self.pump(0.05)
        assert rc == code, (rc, code, self.screen.raw[-400:])
        assert termios.tcgetattr(self.slave) == self.original
        os.close(self.master)
        os.close(self.slave)
        checks += 1


def lines(n, width=8):
    return b''.join(b'line%03d\n' % i for i in range(n))


def run(tmp):
    global checks
    root = pathlib.Path(tmp)
    hundred = root / 'hundred'
    hundred.write_bytes(lines(100))
    ready = lambda p: p.top_byte() is not None

    # Counts before commands, percent and byte jumps, line numbers.
    p = Pager([str(hundred)])
    p.wait(ready, 'first screen')
    p.expect_top('5j', 40)
    p.expect_top('3k', 16)
    p.expect_top('10f', 96)
    p.expect_top('2b', 80)
    p.expect_top('50%', 400)
    p.expect_top('p', 0)
    p.expect_top('25p', 200)
    p.expect_top('123P', 120)
    p.expect_top('42g', 328)
    p.expect_top('g', 0)
    p.expect_top('10G', 72)
    p.expect_top('G', 93 * 8)
    p.expect_top('1g', 0)
    p.expect_status('500g', 'past the end')
    p.expect_top('g3z', 24)
    p.expect_top('z', 48)
    p.expect_top('w', 24)
    p.expect_top('2d', 40)
    p.expect_top('d', 56)
    p.expect_top('u', 40)
    # Marks, the previous position, and the bottom-line mark.
    p.expect_top('ma', 40)
    p.expect_top('G', 744)
    p.expect_top("'a", 40)
    p.expect_top("''", 744)
    p.expect_top("\x18\x18'", 40)
    p.expect_top('gMb', 0)
    p.expect_top("50%'b", 0)
    p.expect_status('\x1bmb\'b', 'Mark not set')
    p.expect_top("'^", 0)
    p.expect_top("'$", 744)
    p.expect_top('g', 0)
    # Forced moves past the end-of-file stop.
    p.expect_top('G\x1b ', 96 * 8)  # 3z set the window to 3 lines
    p.expect_top('\x1b \x1b ', 99 * 8)
    p.expect_top('gJ', 8)
    p.expect_top('K', 0)
    p.expect_status('=', 'byte 0')
    p.expect_status('V', 'fastless 1.0')
    p.quit()

    # Horizontal scrolling, the truncation marker, and header columns.
    wide = root / 'wide'
    wide.write_bytes(b''.join(b'%02d:' % i + bytes(range(65, 91)) * 4 + b'\n' for i in range(20)))
    def view(n, shift, header=0, width=39, marker='>'):
        text = '%02d:' % n + 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' * 4
        body = text[header + shift:]
        room = width - header
        if len(body) > room:
            body = body[:room - 1] + marker if marker else body[:room]
        return text[:header] + body
    p = Pager([str(wide)], cols=40)
    p.wait(ready)
    p.expect_row('', 0, view(0, 0))
    p.expect_row('\x1b)', 0, view(0, 19))
    p.expect_row('\x1b[C', 0, view(0, 38))
    p.expect_row('\x1b(', 0, view(0, 19))
    p.expect_row('\x1b}', 0, view(0, 68))
    p.expect_row('\x1b{', 0, view(0, 0))
    p.expect_row('5\x1b)', 0, view(0, 5))
    p.expect_row('\x1b)', 0, view(0, 10))
    p.expect_row('', 3, view(3, 10))
    p.quit()
    p = Pager(['--header=1,3', '--rscroll=-', '-#4', str(wide)], cols=40)
    p.wait(ready)
    p.expect_row('\x1b)', 0, view(0, 4, 3, marker=''))
    p.expect_row('', 1, view(1, 4, 3, marker=''))
    p.expect_row('3j', 1, view(4, 4, 3, marker=''))
    p.expect_row('', 0, view(0, 4, 3, marker=''))
    p.quit()
    # A match off to the right shifts the view, keeping --match-shift columns.
    far = root / 'far'
    far.write_bytes(b'x' * 100 + b'target' + b'y' * 20 + b'\n' + b'cols ab target\n')
    p = Pager(['--match-shift=5', str(far)], cols=40)
    p.wait(ready)
    p.expect_row('/target\r', 0, 'xxxxxtarget' + 'y' * 20)
    p.expect_row('\x1b{', 1, 'cols ab target')
    p.quit()
    p = Pager(['--header=0,8', '--no-search-header-columns', str(far)])
    p.wait(ready)
    p.expect_status('/ab\r', 'Pattern not found')
    p.expect_status('g/tar\r', 'Match at byte 100')
    p.quit()
    # Header lines from a later line stay while scrolling; searches can skip them.
    p = Pager(['--header=2,0,11', '--no-search-header-lines', str(hundred)])
    p.wait(ready)
    p.expect_row('', 0, 'line010')
    p.expect_row('', 1, 'line011')
    p.expect_row('', 2, 'line012')
    p.expect_row('20j', 0, 'line010')
    p.expect_status('?line011\r', 'Pattern not found')
    p.quit()
    # --no-search-headers skips both header lines and columns, as in less.
    p = Pager(['--header=2,0,11', '--no-search-headers', str(hundred)])
    p.wait(ready)
    p.expect_status('20j?line011\r', 'Pattern not found')
    p.expect_status('__no-search-headers\r', 'header lines: on, columns: on')
    p.quit()

    # Characters: UTF-8, control and invalid bytes, overstrike, CR, tabs, color.
    chars = root / 'chars'
    chars.write_bytes('héllo wörld 日本語 end\n'.encode() + b'ctl\x01\x7f bad\xff c1\xc2\x85 x\n'
                      b'b\bbo\bol\bld _\bu_\bn\n' + b'dos\r\n' + b'a\rb\n' + b'a\tb\tc\n' +
                      b'\x1b[31mred\x1b[0m plain\n')
    p = Pager([str(chars)])
    p.wait(ready)
    p.expect_row('', 0, 'héllo wörld 日本語 end')
    p.expect_row('', 1, 'ctl^A^? bad<FF> c1<U+0085> x')
    p.expect_row('', 2, 'bold un')
    assert 'bold' in p.screen.style[2][0] and 'under' in p.screen.style[2][5], p.screen.style[2][:7]
    assert 'bold' in p.screen.style[1][3], 'control characters are bold'
    p.expect_row('', 3, 'dos')
    p.expect_row('', 4, 'a^Mb')
    p.expect_row('', 5, 'a       b       c')
    p.expect_row('', 6, '^[[31mred^[[0m plain')
    p.quit()
    p = Pager(['-R', '-x4', '--PROC-RETURN', '-U', str(chars)], env={'LESSCHARSET': 'ascii'})
    p.wait(ready)
    p.expect_row('', 0, 'h<C3><A9>llo w<C3><B6>rld <E6><97><A5><E6><9C><AC><E8><AA><9E> end')
    p.expect_row('', 2, 'b^Hbo^Hol^Hld _^Hu_^Hn')
    p.expect_row('', 3, 'dos^M')
    p.expect_row('', 5, 'a^Ib^Ic')
    p.expect_row('', 6, 'red plain')
    assert b'\x1b[31mred\x1b[0m' in p.screen.raw
    assert 'color' in p.screen.style[6][0]
    p.quit()
    p = Pager(['-x4,10', str(chars)])
    p.wait(ready)
    p.expect_row('', 5, 'a   b     c')
    p.quit()

    # Squeezed blank lines, tildes, the status column, and unread highlighting.
    blanks = root / 'blanks'
    blanks.write_bytes(b'a\n\n\n\nb\n\n\nc\n')
    p = Pager(['-s', '-~', str(blanks)])
    p.wait(ready)
    assert p.lines()[:6] == ['a', '', 'b', '', 'c', ''], p.lines()
    p.quit()
    p = Pager(['-s', str(blanks)], rows=4)
    p.wait(ready)
    p.expect_top('j', 2)
    p.expect_top('j', 5)
    p.expect_top('k', 2)
    p.quit()
    p = Pager(['-J', '-w', str(hundred)])
    p.wait(ready)
    p.expect_row('maj', 0, '  line001')
    p.expect_row('k', 0, 'a line000')
    p.expect_row('/line00[24]\r', 0, '* line002')
    p.expect_row('', 2, '* line004')
    p.expect_row(' ', 0, '  line009')
    assert 'rev' in p.screen.style[0][3], 'the first new line is highlighted'
    p.quit()

    p = Pager(['-W', '--status-line', str(hundred)])
    p.wait(ready)
    p.expect_top('mzj', 8)
    assert 'rev' in p.screen.style[6][0], '-W highlights the new line after j'
    p.expect_top('k', 0)
    assert 'rev' in p.screen.style[0][0], '--status-line highlights marked lines'
    checks += 2
    p.quit()
    # Highlighting: -g only the found match, -G none, ESC-u toggles.
    two = root / 'two'
    two.write_bytes(b'one needle two needle\n')
    for options, expect in (([], ['rev', 'rev']), (['-g'], ['rev', None]), (['-G'], [None, None])):
        p = Pager(options + [str(two)])
        p.wait(ready)
        p.expect_status('/needle\r', 'Match at byte 4')
        got = ['rev' if 'rev' in p.screen.style[0][i] else None for i in (4, 15)]
        assert got == expect, (options, got)
        checks += 1
        if not options:
            p.keys('\x1bu')
            p.wait(lambda q: 'rev' not in q.screen.style[0][4], 'highlight off')
            p.keys('\x1bu')
            p.wait(lambda q: 'rev' in q.screen.style[0][4], 'highlight on')
            p.keys('\x1bU')
            p.wait(lambda q: 'rev' not in q.screen.style[0][4], 'pattern cleared')
            p.expect_status('n', 'No search pattern')
        p.quit()

    # Search targets, counts, and where searches start.
    p = Pager(['-j3', str(hundred)])
    p.wait(ready)
    p.expect_top('/line050\r', 48 * 8)
    p.expect_top('g2/line0[0-9]5\r', 13 * 8)
    p.expect_top('3n', 43 * 8)
    p.expect_top('N', 33 * 8)
    p.quit()
    p = Pager(['-j.5', str(hundred)])
    p.wait(ready)
    p.expect_top('/line050\r', 47 * 8)
    p.quit()
    p = Pager(['-a', str(hundred)])
    p.wait(ready)
    p.expect_status('/line00[0-9]\r', 'Match at byte 56')
    p.quit()
    p = Pager(['-A', str(hundred)])
    p.wait(ready)
    p.expect_status('/line00[0-9]\r', 'Match at byte 8')
    p.quit()

    # Search modifiers typed at the start of the pattern.
    mods = root / 'mods'
    mods.write_bytes(b'xx\nxx\nyy\nabc a.c\nfoobar fooqux\nxx\n')
    p = Pager([str(mods)])
    p.wait(ready)
    p.expect_status('/\x0exx\r', 'Match at byte 6')
    p.expect_status('n', 'Match at byte 9')
    p.expect_status('g/\x12a.c\r', 'Match at byte 13')
    p.expect_status('g/\x13' + '1foo(qux)\r', 'Match at byte 27')
    p.expect_status('g/!xx\r', 'Match at byte 6')
    p.expect_top('G/\x0byy\r', 0)
    p.wait(lambda q: 'rev' in q.screen.style[2][0], 'kept position with highlighting')
    p.expect_status('', 'q quit')
    p.expect_status('?yy\r', 'Pattern not found')
    p.expect_status('?\x17yy\r', '(wrapped) Match at byte 6')
    p.expect_status('g/\x17abc\r', 'Match at byte 9')
    p.quit()
    p = Pager(['--search-options=W', str(mods)])
    p.wait(ready)
    p.expect_status('G?yy\r', 'Match at byte 6')
    p.quit()

    # Prompt line editing, cancelling, and search history.
    hist = root / 'history'
    p = Pager([str(hundred)], env={'FASTLESSHISTFILE': str(hist)})
    p.wait(ready)
    p.expect_top('/line05x\x7f0\r', 400)
    p.expect_top('g/ine060\x1b[Hl\r', 480)
    p.expect_top('g/lin79\x1b[D\x1b[De0\x1b[C\r', 632)
    p.expect_top('g/garbage\x15line020\r', 160)
    p.expect_top('g/line030\x1b', 0)
    p.expect_top('/\x1b[A\r', 160)
    p.expect_top('g/\x1b[A\x1b[A\r', 632)
    p.expect_top('g/line04\x1bb\x1bX0\x1b$45\r', 360, 'word editing')  # searches 045
    p.quit()
    assert hist.exists() and b'"line020' in hist.read_bytes() and b'.search' in hist.read_bytes()
    checks += 1

    # Filtering with &, inverted filters, and searching within them.
    p = Pager([str(hundred)])
    p.wait(ready)
    p.expect_row('&5\r', 0, 'line005')
    p.expect_row('', 1, 'line015')
    p.expect_row('j', 0, 'line015')
    p.expect_row('G', 6, 'line095')
    p.expect_row('g/line05\r', 0, 'line050')
    p.expect_row('j', 0, 'line051')
    p.expect_row('&\r', 1, 'line052')
    p.expect_row('g&\x0e[0-8]$\r', 0, 'line009')
    p.expect_row('', 1, 'line019')
    p.quit()

    # Bracket matching.
    code = root / 'code'
    code.write_bytes(b'{ ( <\n' + b'body\n' * 20 + b'> ) }\nend\n' + b'tail\n' * 20)
    p = Pager([str(code)])
    p.wait(ready)
    p.expect_row('{', 6, '> ) }')
    p.expect_row('}', 0, '{ ( <')
    p.expect_row('(', 6, '> ) }')
    p.expect_row('g\x1b\x06<>', 6, '> ) }')
    p.expect_row('\x1b\x02<>', 0, '{ ( <')
    p.expect_status('j{', 'No bracket')
    p.quit()

    # Quitting rules: -e, -E, -F, -K, and -X.
    short = root / 'short'
    short.write_bytes(b'alpha\nbeta\n')
    p = Pager(['-F', str(short)])
    p.quit(0, key=None)
    assert b'alpha\nbeta\n' in p.screen.raw.replace(b'\r', b'') and b'1049h' not in p.screen.raw
    p = Pager(['-e', str(hundred)])
    p.wait(ready)
    p.expect_top('G', 744)
    p.keys(' ')
    p.quit(0, key=None)
    p = Pager(['-E', str(hundred)])
    p.wait(ready)
    p.expect_top('90j', 720)
    p.keys('3j')
    p.quit(0, key=None)
    p = Pager(['-K', str(hundred)], setsid=True)
    p.wait(ready)
    p.keys('\x03')
    p.quit(2, key=None)
    p = Pager(['-X', '--redraw-on-quit', str(hundred)])
    p.wait(ready)
    p.quit()
    assert b'\x1b[?1049h' not in p.screen.raw
    p = Pager(['--redraw-on-quit', str(hundred)])
    p.wait(ready)
    p.keys('5j')
    p.wait(lambda q: q.top_byte() == 40)
    p.quit()
    assert not p.screen.alt and p.screen.line(0).startswith('line005'), p.lines()
    p = Pager(['-c', str(hundred)])
    p.wait(ready)
    p.quit()
    assert b'\x1b[2J' in p.screen.raw
    # The bell rings at the ends, except with -q (movement) or -Q (always).
    for options, bells in (([], 2), (['-q'], 1), (['-Q'], 0)):
        p = Pager(options + [str(hundred)])
        p.wait(ready)
        p.keys('k')
        p.keys('\x1bx')
        p.pump(0.3)
        assert p.screen.bells == bells, (options, p.screen.bells)
        checks += 1
        p.quit()

    # Window size, prompts, and the = message.
    p = Pager(['-z3', str(hundred)])
    p.wait(ready)
    p.expect_top(' ', 24)
    p.quit()
    p = Pager(['-z-2', str(hundred)])
    p.wait(ready)
    p.expect_top(' ', 40)
    p.quit()
    p = Pager(['-Ps%f at %bt of %B?e (END).', str(short)])
    p.wait(lambda q: 'short at 0 of 11 (END)' in q.status())
    p.quit()
    # Prompts as less 668 shows them for the same files and keys.
    p = Pager(['-m', 'hundred'], cwd=root)
    p.wait(lambda q: q.status() == 'hundred 7%')
    p.expect_status('5j', '12%')
    p.expect_status('=', 'hundred lines 6-12/100 byte 96/800 12%')
    p.expect_status('G', '(END)')
    p.quit()
    p = Pager(['-M', 'hundred', 'short'], cwd=root)
    p.wait(lambda q: q.status() == 'hundred (file 1 of 2) lines 1-7/100 7%')
    p.expect_status('5j', 'hundred lines 6-12/100 12%')
    p.expect_status(':n', 'short (file 2 of 2) lines 1-2/2 (END)')
    p.expect_status(':p', 'hundred (file 1 of 2) lines 6-12/100 12%')
    p.quit()
    # -n suppresses line numbers in prompts; page and line-percent items.
    p = Pager(['-n', '-M', 'hundred'], cwd=root)
    p.wait(lambda q: q.status() == 'hundred byte 56/800 7%')
    p.quit()
    p = Pager(['-Ps%f page %dt of %D, %Pb%% ?L%L lines.', 'hundred'], cwd=root)
    p.wait(lambda q: q.status() == 'hundred page 1 of 15, 7% 100 lines')
    p.expect_status('50%', 'hundred page 8 of 15, 57% 100 lines')
    p.quit()

    # The -N column: numbers follow the lines shown, widen as needed, and
    # number header lines unless --no-number-headers.
    p = Pager(['-N', str(hundred)])
    p.wait(ready)
    p.expect_row('', 0, '      1 line000')
    p.expect_row('5j', 0, '      6 line005')
    p.expect_row('G', 6, '    100 line099')
    p.expect_row('&5\r', 0, '     96 line095')
    p.expect_row('g', 1, '     16 line015')
    p.expect_row('&\r', 1, '      7 line006')
    p.expect_row('-N', 0, 'line005')
    p.expect_row('-N', 0, '      6 line005')
    p.quit()
    # The column is as wide as the largest number on the screen needs.
    p = Pager(['-N', '-J', '--line-num-width=1', '--header=1', str(hundred)])
    p.wait(ready)
    p.expect_row('', 0, '  1 line000')
    p.expect_row('ma', 1, 'a 2 line001')
    p.expect_row('G', 6, '  100 line099')
    p.expect_row('', 0, '    1 line000')
    p.expect_row('g', 1, 'a 2 line001')
    p.quit()
    p = Pager(['-N', '--header=1', '--no-number-headers', '--line-num-width=3', str(hundred)])
    p.wait(ready)
    p.expect_row('', 0, '    line000')
    p.expect_row('', 1, '  2 line001')
    p.quit()
    p = Pager(['-F', '-N', '--line-num-width=2', str(short)])
    p.quit(0, key=None)
    assert b' 1 alpha\n 2 beta\n' in p.screen.raw.replace(b'\r', b''), p.screen.raw
    checks += 1
    # Past the few pieces counted at once (64 MiB each), counting happens
    # between keystrokes; the numbers appear once known.
    many = root / 'many'
    block = b''.join(b'%07d padding padding padding padding padding\n' % i for i in range(100000))
    blocks = 200 * 1024**2 // len(block) + 1
    with many.open('wb') as f:
        for _ in range(blocks):
            f.write(block)
    total = blocks * 100000
    p = Pager(['-N', str(many)])
    p.wait(ready)
    p.keys('G')
    p.wait(lambda q: q.screen.line(6) == '%d %07d padding padding padding padding padding' % (total, (total - 1) % 100000),
           'numbers at the end of a large file', timeout=30)
    p.expect_row('g', 0, '      1 0000000 padding padding padding padding padding')
    p.expect_row('3000000g', 0, '3000000 %07d padding padding padding padding padding' % (2999999 % 100000))
    p.quit()

    # Multiple files, :e, :d, and searches that span files.
    third = root / 'third'
    third.write_bytes(b'third file\nmore\n')
    p = Pager([str(hundred), str(short)])
    p.wait(ready)
    p.expect_status(':n', 'short (file 2 of 2)')
    p.expect_status(':p', 'hundred (file 1 of 2)')
    p.expect_status('2:x', 'short (file 2 of 2)')
    p.expect_status(':x', 'hundred (file 1 of 2)')
    p.expect_status('/beta\r', 'Pattern not found')
    p.expect_status('\x1bn', 'short (file 2 of 2)')
    p.expect_status('', 'Match at byte 6')
    p.expect_status(':e ' + str(third) + '\r', 'third (file 3 of 3)')
    p.expect_status(':d', '(file 2 of 2)')
    p.expect_status(':p', 'hundred (file 1 of 2)')
    p.expect_status('/\x05beta\r', 'short (file 2 of 2)')
    p.expect_status('/\x05\x06line\r', 'hundred (file 1 of 2)')
    p.expect_status('', 'Match at byte 0')
    p.expect_status(':n?\x05\x06beta\r', 'Match at byte 6')
    p.expect_status(':p50%:n', 'short')
    p.expect_status(':p', 'byte 400/800')
    p.quit()

    # Cached line bounds must not outlive their document.
    longline, small = root / 'longline', root / 'small'
    longline.write_bytes(b'y' * 3000 + b'needle\n')
    small.write_bytes(b'short\nneedle here\n')
    p = Pager(['--threads', '1', 'longline', 'small'], cwd=root)
    p.wait(ready)
    p.expect_status('/ne+dle\r', 'Match at byte 3000')
    p.expect_status(':n', 'small')
    p.expect_status('/^ne+dle\r', 'Match at byte 6')
    p.quit()

    # Options while running: -, --, _, -+.
    p = Pager([str(two)])
    p.wait(ready)
    p.expect_status('-I', 'Ignore case in all searches: on')
    p.expect_status('/NEEDLE\r', 'Match at byte 4')
    p.expect_status('-+I', 'off')
    p.expect_status('_I', 'off')
    p.expect_status('--squ\r', 'Squeeze multiple blank lines: on')
    p.expect_status('__squeeze-blank-lines\r', 'on')
    p.expect_status('-x', '--tabs=')
    p.expect_status('4\r', 'Tab stops')
    p.expect_status('-X', 'cannot be changed')
    p.expect_status('-D', 'does not support')
    p.expect_status('--nosuch\r', 'There is no --nosuch option')
    p.expect_status('--literal\r', 'Literal searches: on')
    p.expect_status('', 'literal')
    p.expect_status('\x1br', 'regex')
    p.expect_status('\x1bw', 'glob')
    p.expect_status('\x1bl', 'literal')
    p.quit()

    # Shell commands, pipes, saving, and editing.
    out = root / 'out'
    p = Pager([str(short)], env={'SHELL': '/bin/sh'})
    p.wait(ready)
    p.keys(f'!echo shell %>{out}\r')
    p.wait(lambda q: 'press RETURN' in q.screen.line(q.screen.rows - 1) or out.exists(), 'shell')
    p.keys('\r')
    p.wait(ready)
    assert out.read_text() == f'shell {short}\n', out.read_text()
    p.keys(f'|^cat >{out}\r')
    p.wait(lambda q: b'press RETURN' in q.screen.raw[-200:], 'pipe')
    p.keys('\r')
    p.wait(ready)
    assert out.read_bytes() == b'alpha\nbeta\n'
    p.keys(f'#echo %f %bt >{out}\r')
    p.wait(lambda q: b'press RETURN' in q.screen.raw[-200:], 'pshell')
    p.keys('\r')
    p.wait(ready)
    assert out.read_text() == f'{short} 0\n', out.read_text()
    saved = root / 'saved'
    p.expect_status(f's{saved}\r', 'Saved')
    assert saved.read_bytes() == short.read_bytes()
    p.expect_status(f's{saved}\rA', 'Saved')
    assert saved.read_bytes() == short.read_bytes() * 2
    checks += 4
    p.quit()
    editor = root / 'editor.sh'
    editor.write_text('#!/bin/sh\necho "$@" > "$0.args"\nfor f; do :; done\necho edited >> "$f"\n')
    editor.chmod(0o755)
    edited = root / 'edited'
    edited.write_bytes(b'before\n')
    p = Pager([str(edited)], env={'VISUAL': str(editor)})
    p.wait(ready)
    p.expect_row('v', 1, 'edited')
    p.quit()
    # As less's default LESSEDIT does, the editor is given the middle line.
    assert pathlib.Path(str(editor) + '.args').read_text() == f'+1 {edited}\n'
    checks += 1
    # An empty VISUAL counts as unset, so EDITOR is used.
    p = Pager([str(edited)], env={'VISUAL': '', 'EDITOR': str(editor)})
    p.wait(ready)
    p.expect_row('v', 2, 'edited')
    p.quit()
    checks += 1
    p = Pager([str(short)], env={'LESSSECURE': '1'})
    p.wait(ready)
    p.expect_status('!', 'Command not available')
    p.quit()

    # Help, startup commands, offsets, and $LESS.
    p = Pager([str(short)])
    p.wait(ready)
    p.expect_row('h', 0, 'SUMMARY OF FASTLESS COMMANDS', exact=False)
    p.expect_status('', 'Help (press q when done)')
    p.expect_row('q', 0, 'alpha')
    p.quit()
    p = Pager(['+G', str(hundred)])
    p.wait(lambda q: q.top_byte() == 744)
    p.quit()
    p = Pager(['+/line050', str(hundred)])
    p.wait(lambda q: q.top_byte() == 400)
    p.quit()
    p = Pager(['-p', 'line060', str(hundred)])
    p.wait(lambda q: q.top_byte() == 480)
    p.quit()
    p = Pager(['++50%', str(hundred), str(short)])
    p.wait(lambda q: q.top_byte() == 400)
    p.expect_status(':n', 'short')
    p.quit()
    p = Pager(['--offset', '83', str(hundred)])
    p.wait(lambda q: q.top_byte() == 80)
    p.quit()
    p = Pager([str(chars)], env={'LESS': '-x4 -P?f%f$ -R'})
    p.wait(lambda q: q.status() == str(chars))
    p.expect_row('', 5, 'a   b   c')
    p.expect_row('', 6, 'red plain')
    p.quit()

    # Key bindings from lesskey source.
    keys = root / 'keys'
    keys.write_text('#command\nx forw-line\n\\kd back-line\nQ noaction\n#env\nLESS = -I\n')
    p = Pager(['--lesskey-src', str(keys), str(two)])
    p.wait(ready)
    p.expect_status('/NEEDLE\r', 'Match at byte 4')
    p.quit()
    p = Pager(['--lesskey-src=' + str(keys), str(hundred)])
    p.wait(ready)
    p.expect_top('xx', 16)
    p.expect_top('\x1b[B', 8)
    p.expect_top('Qj', 16)
    p.quit()
    p = Pager(['--lesskey-content=#command;t forw-screen', str(hundred)])
    p.wait(ready)
    p.expect_top('t', 56)
    p.quit()
    # LESSKEY_CONTENT too; a backslash before a semicolon makes it literal.
    p = Pager([str(hundred)], env={'LESSKEY_CONTENT': '#command;\\; forw-screen'})
    p.wait(ready)
    p.expect_top(';', 56)
    p.quit()

    # Mouse wheel scrolling.
    p = Pager(['--mouse', '--wheel-lines=3', str(hundred)])
    p.wait(ready)
    assert b'\x1b[?1000h' in p.screen.raw
    p.expect_top('\x1b[<65;1;1M', 24)
    p.expect_top('\x1b[<64;1;1M', 0)
    # A click marks the line at row 2, and '# shows it there again.
    p.expect_top('\x1b[<65;1;1M\x1b[<0;1;2M\x1b[<0;1;2mG\'#', 24)
    p.expect_row('', 1, 'line004')
    p.quit()

    # Incremental search shows the match while the pattern is typed.
    p = Pager(['--incsearch', str(hundred)])
    p.wait(ready)
    p.keys('/line07')
    p.wait(lambda q: q.screen.line(0) == 'line070' and q.status().startswith('/line07'), 'incremental match')
    p.keys('5')
    p.wait(lambda q: q.screen.line(0) == 'line075', 'refined match')
    p.keys('\x1b')
    p.wait(lambda q: q.top_byte() == 0, 'cancelled search restores the position')
    checks += 3
    p.quit()

    # Follow mode: F shows appended data; ESC-F stops at a match.
    grow = root / 'grow'
    grow.write_bytes(lines(20))
    p = Pager([str(grow)], setsid=True)
    p.wait(ready)
    p.keys('F')
    p.wait(lambda q: 'Waiting for data' in q.status())
    with grow.open('ab') as f:
        f.write(b'appended\n')
    p.wait(lambda q: 'appended' in q.lines(), 'appended line')
    p.keys('\x03')
    p.wait(lambda q: 'Waiting' not in q.status() and q.top_byte() is not None)
    p.expect_status('g/line010\r', 'Match at byte 80')
    p.keys('\x1bF')
    p.wait(lambda q: 'Waiting for data' in q.status())
    with grow.open('ab') as f:
        f.write(b'filler\n' * 30 + b'has line010 again\n' + b'after\n' * 30)
    p.wait(lambda q: 'Match at byte' in q.status() and q.screen.line(0) == 'has line010 again', 'ESC-F match')
    checks += 3
    p.quit()
    renamed = root / 'renamed'
    p = Pager(['--follow-name', str(grow)])
    p.wait(ready)
    p.keys('F')
    p.wait(lambda q: 'Waiting for data' in q.status())
    renamed.write_bytes(b'replacement file\n')
    os.rename(renamed, grow)
    p.wait(lambda q: 'replacement file' in q.lines(), 'renamed file')
    p.keys('q')
    p.quit(0, key=b'q')

    # Standard input, FIFOs, devices, and directories.
    p = Pager([], stdin=subprocess.PIPE, setsid=True)
    p.child.stdin.write(lines(30))
    p.child.stdin.close()
    p.wait(lambda q: q.screen.line(0) == 'line000', 'piped input')
    p.expect_top('/line020\r', 160)
    p.expect_status('=', 'byte 216/240 90%')  # no name for a pipe, as in less
    p.quit()
    # Pipes are shown while they are read, as in less.
    p = Pager([], stdin=subprocess.PIPE, setsid=True)
    p.child.stdin.write(b'first\nsecond\n')
    p.child.stdin.flush()
    p.wait(lambda q: q.screen.line(1) == 'second' and '(reading)' in q.status(), 'lines before the end')
    p.expect_status('=', 'lines 1-2 byte 13')
    assert p.status().endswith('byte 13'), p.status()  # sizes are unknown until the end
    p.child.stdin.write(b'third\n')
    p.child.stdin.flush()
    p.expect_row('', 2, 'third')
    p.keys(' ')  # forward at the end waits for more
    p.wait(lambda q: 'Waiting for data' in q.status(), 'forward wait')
    p.child.stdin.write(lines(20)[8:])
    p.child.stdin.flush()
    p.wait(lambda q: q.top_byte() > 0 and 'Waiting' not in q.status(), 'forward after more input')
    p.keys('/needle\r')  # searches continue into arriving input
    p.wait(lambda q: 'as it arrives' in q.status(), 'search waiting')
    p.child.stdin.write(b'hay\nthe needle\n')
    p.child.stdin.flush()
    p.expect_row('', 0, 'the needle')
    p.expect_status('/absent\r', 'as it arrives')
    p.expect_status('\x03', 'Search cancelled')
    p.keys('G')  # G follows the end until the input ends
    p.wait(lambda q: 'Reading to the end' in q.status(), 'G waiting')
    p.child.stdin.write(b'last line\n')
    p.child.stdin.flush()
    p.expect_row('', 6, 'last line')
    p.child.stdin.close()
    p.expect_status('', '(END)')
    p.expect_status('=', '/196')
    p.quit()
    # Quitting while the writer is still open, and following a pipe with F.
    p = Pager([], stdin=subprocess.PIPE, setsid=True)
    p.child.stdin.write(lines(3))
    p.child.stdin.flush()
    p.expect_row('', 2, 'line002')
    p.keys('F')
    p.wait(lambda q: 'Waiting for data' in q.status(), 'following a pipe')
    p.child.stdin.write(lines(12)[24:])
    p.child.stdin.flush()
    p.expect_row('', 6, 'line011')
    p.keys('q')
    p.wait(lambda q: 'Waiting' not in q.status(), 'F ends')
    p.quit()
    p.child.stdin.close()
    # A compressed pipe is read whole, then decoded.
    blob = gzip.compress(lines(30))
    p = Pager([], stdin=subprocess.PIPE, setsid=True)
    p.child.stdin.write(blob[:12])
    p.child.stdin.flush()
    p.expect_status('', 'reading compressed input')
    p.child.stdin.write(blob[12:])
    p.child.stdin.close()
    p.expect_row('', 0, 'line000')
    p.expect_status('', '(0.0%)')
    p.quit()
    # -F waits until a pipe is known to fit.
    r = subprocess.run(['sh', '-c', f'(echo one; sleep 0.3; echo two) | "{EXE}" -F'], capture_output=True,
                       timeout=10, env=dict(os.environ, LESS='', LESSOPEN=''))
    assert r.returncode == 0 and r.stdout == b'one\ntwo\n', r
    checks += 1
    # Cat mode copies a pipe as it arrives.
    cat = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           env=dict(os.environ, LESS='', LESSOPEN=''))
    cat.stdin.write(b'early\n')
    cat.stdin.flush()
    assert select.select([cat.stdout], [], [], 5)[0] and cat.stdout.readline() == b'early\n'
    cat.stdin.write(b'late\n')
    cat.stdin.close()
    assert cat.stdout.read() == b'late\n' and cat.wait(timeout=5) == 0
    checks += 1
    # Beyond --max-mem (and 64 MiB), a pipe is read only as far as needed.
    # The search reads on until stopped: progress shows, and a key stops it.
    yes = subprocess.Popen(['yes', 'y' * 99], stdout=subprocess.PIPE)
    p = Pager(['--max-mem', '0', '--tmpdir', str(root)], stdin=yes.stdout, setsid=True)
    yes.stdout.close()
    try:
        read = lambda q: int(re.search(r'byte \d+/(\d+)', q.status()).group(1))
        p.wait(lambda q: q.screen.line(0).startswith('yyy') and read(q) >= 64 << 20, 'read-ahead')
        time.sleep(0.5)
        p.keys('R')
        p.wait(lambda q: 'reading' in q.status())
        assert read(p) < 80 << 20, read(p)
        p.keys('/absent\r')
        p.wait(lambda q: 'as it arrives' in q.status() and read(q) > 200 << 20, 'reading on demand', timeout=30)
        p.keys('g')  # stops the search, then runs
        p.wait(lambda q: 'as it arrives' not in q.status() and 'help' in q.status(), 'a key stops the search')
        sizes = []
        for _ in range(2):
            time.sleep(0.4)
            p.keys('R')
            p.pump(0.2)
            sizes.append(read(p))
        assert sizes[1] - sizes[0] < 2 << 20, sizes
        p.quit()
    finally:
        if p.child.poll() is None:
            p.child.kill()
            p.child.wait()
        yes.kill()
        yes.wait()
    assert not [f for f in root.iterdir() if f.name.startswith('fastless-')]
    checks += 2
    fifo = root / 'fifo'
    os.mkfifo(fifo)
    writer = threading.Thread(target=lambda: fifo.write_bytes(b'from a fifo\n'))
    writer.start()
    p = Pager([str(fifo)])
    p.wait(lambda q: q.screen.line(0) == 'from a fifo', 'fifo input')
    writer.join()
    p.quit()
    # A pipe stays open while another file is shown: it cannot be read again.
    writer = threading.Thread(target=lambda: fifo.write_bytes(b'kept from a fifo\n'))
    writer.start()
    p = Pager([str(fifo), str(short)])
    p.wait(lambda q: q.screen.line(0) == 'kept from a fifo', 'fifo input')
    writer.join()
    p.expect_row(':n', 0, 'alpha')
    p.expect_row(':p', 0, 'kept from a fifo')
    p.expect_row(':e\r', 0, 'kept from a fifo')
    p.quit()
    checks += 1
    # Cat mode copies a pipe through a buffer, keeping none of it.
    cat = subprocess.Popen([EXE, '--max-mem', '0', '--tmpdir', str(root)], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, env=dict(os.environ, LESS='', LESSOPEN=''))
    copied = []
    reader = threading.Thread(target=lambda: copied.append(sum(iter(lambda: len(cat.stdout.read(1 << 20)), 0))))
    reader.start()
    chunk = b'x' * 1023 + b'\n'
    for _ in range(256):
        cat.stdin.write(chunk * 1024)
        assert not [f for f in root.iterdir() if f.name.startswith('fastless-')]
    cat.stdin.close()
    reader.join()
    rss = os.wait4(cat.pid, 0)[2].ru_maxrss
    cat.returncode = 0
    assert copied == [256 << 20] and rss < 48 << 10, (copied, rss)
    writer = threading.Thread(target=lambda: fifo.write_bytes(b'copied from a fifo\n'))
    writer.start()
    r = subprocess.run([EXE, str(fifo)], capture_output=True, timeout=5, env=dict(os.environ, LESS='', LESSOPEN=''))
    writer.join()
    assert r.returncode == 0 and r.stdout == b'copied from a fifo\n', r
    checks += 2
    for args, text in ((['/dev/null'], 'not a regular file'), ([str(root)], 'is a directory')):
        r = subprocess.run([EXE, *args], capture_output=True, timeout=5,
                           env=dict(os.environ, LESS='', LESSOPEN=''))
        assert r.returncode == 2 and text.encode() in r.stderr, r
        checks += 1
    r = subprocess.run([EXE, '-f', '/dev/null'], capture_output=True, timeout=5,
                       env=dict(os.environ, LESS='', LESSOPEN=''))
    assert r.returncode == 0 and r.stdout == b'', r
    checks += 1

    # Input preprocessors (LESSOPEN/LESSCLOSE).
    p = Pager([str(short)], env={'LESSOPEN': "|tr a-z A-Z <%s"})
    p.wait(lambda q: q.screen.line(0) == 'ALPHA', 'pipe preprocessor')
    p.quit()
    p = Pager(['-L', str(short)], env={'LESSOPEN': "|tr a-z A-Z <%s"})
    p.wait(lambda q: q.screen.line(0) == 'alpha', '-L ignores LESSOPEN')
    p.quit()
    p = Pager([str(short)], env={'LESSOPEN': "|true %s"})
    p.wait(lambda q: q.screen.line(0) == 'alpha', 'empty output uses the original')
    p.quit()
    p = Pager([str(short)], env={'LESSOPEN': "||true %s"})
    p.wait(lambda q: q.top_byte() == 0 and q.screen.line(0) == '~', '|| with status 0 is empty')
    p.quit()
    closed = root / 'closed'
    repl = root / 'replacement'
    repl.write_bytes(b'replaced\n')
    p = Pager([str(short)], env={'LESSOPEN': f"echo {repl}; : %s", 'LESSCLOSE': f"echo %s %s >{closed}"})
    p.wait(lambda q: q.screen.line(0) == 'replaced', 'replacement file')
    p.quit()
    assert closed.read_text().split() == [str(short), str(repl)], closed.read_text()
    r = subprocess.run([EXE, '--find', 'ALPHA', str(short)], capture_output=True, timeout=5,
                       env=dict(os.environ, LESS='', LESSOPEN='|tr a-z A-Z <%s'))
    assert r.returncode == 1, r
    checks += 1
    packed = root / 'packed'
    packed.write_bytes(gzip.compress(b'compressed text\n'))
    p = Pager([str(packed)], env={'LESSOPEN': "|echo preprocessed; true %s"})
    p.wait(lambda q: q.screen.line(0) == 'compressed text', 'native decoding skips LESSOPEN')
    p.quit()
    checks += 1

    # Suspension leaves and re-enters the screen; R picks up file growth.
    p = Pager([str(grow)])
    p.wait(ready)
    before = len(p.screen.raw)
    p.child.send_signal(signal.SIGTSTP)
    p.wait(lambda q: q.screen.raw.count(b'\x1b[?1049l', before) >= 1, 'leaving the screen')
    # The process stops unless its process group is orphaned (as under some
    # job runners), when the kernel discards the stop and it resumes at once.
    stat = pathlib.Path(f'/proc/{p.child.pid}/stat')
    state = lambda: stat.read_text().rsplit(')', 1)[1].split()[0] if stat.exists() else '?'
    deadline = time.monotonic() + 1
    while state() != 'T' and time.monotonic() < deadline:
        time.sleep(0.02)
    if state() == 'T':
        assert termios.tcgetattr(p.slave)[3] & termios.ICANON, 'terminal restored while stopped'
        p.child.send_signal(signal.SIGCONT)
    p.wait(lambda q: q.screen.raw.count(b'\x1b[?1049h', before) >= 1 and q.top_byte() is not None, 'resuming')
    checks += 2
    size = grow.stat().st_size
    with grow.open('ab') as f:
        f.write(b'more\n')
    p.expect_status('R', f'/{size + 5}')
    p.quit()

    # Marks saved across sessions.
    marks = root / 'marks'
    p = Pager(['--save-marks', str(hundred)], env={'FASTLESSHISTFILE': str(marks)})
    p.wait(ready)
    p.keys('50%mz')
    p.wait(lambda q: q.top_byte() == 400)
    p.quit()
    p = Pager(['--save-marks', str(hundred)], env={'FASTLESSHISTFILE': str(marks)})
    p.wait(ready)
    p.expect_top("'z", 400)
    p.quit()

    # Cat mode keeps working with several files and preprocessing.
    r = subprocess.run([EXE, str(short), str(packed)], capture_output=True, timeout=5,
                       env=dict(os.environ, LESS='', LESSOPEN=''))
    assert r.stdout == b'alpha\nbeta\ncompressed text\n', r
    checks += 1
    # String options from $LESS keep their values after it is parsed.
    r = subprocess.run([EXE, str(packed)], capture_output=True, timeout=5,
                       env=dict(os.environ, LESS=f'--max-mem=0 --tmpdir={root}/missing', LESSOPEN=''))
    assert r.returncode == 2 and f'in {root}/missing:'.encode() in r.stderr, r
    checks += 1
    # A search after the file is rewritten at its old size sees the new lines.
    edited = root / 'edited'
    edited.write_bytes(b'ab needle\n')
    p = Pager([str(edited)])
    p.wait(ready)
    p.expect_status('/^needle\r', 'Pattern not found')
    with edited.open('r+b') as f:
        f.write(b'ab\nneedle\n')
    p.expect_row(':e\r', 1, 'needle')
    p.expect_status('/^needle\r', 'Match at byte 3')
    p.quit()
    checks += 1


with tempfile.TemporaryDirectory() as tmp:
    run(tmp)
print(f'{checks} pager checks passed')
