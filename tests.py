#!/usr/bin/env python3
"""Integration tests, including mmap offsets beyond 32-bit and terminal cleanup."""
import fnmatch
import os
import pathlib
import random
import select
import subprocess
import tempfile
import time
import pty
import signal
import termios
import fcntl
import struct

EXE = os.environ.get('FASTLESS_TEST_EXE', str(pathlib.Path(__file__).with_name('fastless').resolve()))
SIMD_FLAGS = (["--simd", os.environ["FASTLESS_TEST_SIMD"]]
              if "FASTLESS_TEST_SIMD" in os.environ else [])
checks = 0

def run(path, pattern, mode='--literal', options=(), expected=None, code=None, env=None):
    global checks
    e = dict(os.environ, LESS='')
    if env:
        e.update(env)
    p = subprocess.run([EXE, *SIMD_FLAGS, mode, '--find', pattern, *options, str(path)],
                       capture_output=True, env=e, timeout=15)
    actual = [int(x) for x in p.stdout.splitlines()]
    if expected is not None:
        assert actual == expected, (pattern, options, actual, expected, p.stderr)
    if code is not None:
        assert p.returncode == code, (p.returncode, p.stderr)
    checks += 1
    return actual

with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / 'data'
    payload = b'Alpha beta\nalpha BETA\nxx alpha yy\nabc123\nabZZc\n\x00alpha\xff\n'
    path.write_bytes(payload)
    run(path, 'alpha', options=('--all',), expected=[11, 25, 48], code=0)
    run(path, 'alpha', options=('--all',), env={'LESS': '-FRiX'}, expected=[0, 11, 25, 48])
    run(path, 'Alpha', env={'LESS': '-i'}, expected=[0])
    run(path, 'ALPHA', env={'LESS': '-I'}, expected=[0])
    run(path, 'Alpha', options=('--offset', '1'), env={'LESS': '-i'}, expected=[], code=1)
    run(path, 'Alpha', options=('--offset', '1', '-I'), expected=[11])
    run(path, 'ALPHA', options=('--case-sensitive',), env={'LESS': '-I'}, expected=[], code=1)
    run(path, '^alpha [A-Z]+$', '--regex', expected=[11])
    run(path, 'alpha', '--regex', options=('--all',), expected=[11, 25, 48])
    run(path, '[', '--regex', expected=[], code=2)
    run(path, 'ab*c', '--glob', options=('--all',), expected=[41])
    run(path, '*alpha*', '--glob', options=('--all',), expected=[11, 22, 47])
    run(path, 'abc???', '--glob', expected=[34])
    run(path, 'beta', options=('--offset', '99999'), expected=[], code=1)
    path.write_bytes(b'ab\nabccc\nabc needle\nno match\n')
    for pattern in ('abc*', 'abc?', 'abc{0,2}', 'abc+?', 'abc+*', '^abc+?'):
        run(path, pattern, '--regex', options=('--all',), expected=[0, 3, 9])
    run(path, 'abc|no', '--regex', options=('--all',), expected=[3, 9, 20])
    run(path, '^abc.*needle$', '--regex', expected=[9])
    run(path, '^abc$', '--regex', expected=[], code=1)
    run(path, 'abc.*needle', '--regex', options=('--offset', '10'), expected=[], code=1)
    run(path, '*needle*', '--glob', expected=[9])
    # Chunk-boundary overlap.
    path.write_bytes(b'x' * (8 * 1024 * 1024 - 2) + b'needle' + b'z')
    run(path, 'needle', expected=[8 * 1024 * 1024 - 2])
    # Random differential coverage against Python's literal / whole-line glob matching.
    rng = random.Random(42)
    for _ in range(50):
        buf = bytes(rng.choice(b'abcABC*?\\\n') for _ in range(300))
        path.write_bytes(buf)
        pat = ''.join(rng.choice('abcABC') for _ in range(rng.randint(1, 6)))
        for fold in (False, True):
            hay = buf.lower() if fold else buf
            needle = pat.encode().lower() if fold else pat.encode()
            expected = [i for i in range(len(buf)) if hay.startswith(needle, i)]
            run(path, pat, options=('--all', '-I') if fold else ('--all',), expected=expected)
        pat = rng.choice(['a*', '*B*', '??', '*', 'a?c*'])
        expected = []
        pos = 0
        for line in buf.split(b'\n'):
            if pos >= len(buf):
                break
            if fnmatch.fnmatchcase(line.decode(), pat):
                expected.append(pos)
            pos += len(line) + 1
        run(path, pat, '--glob', options=('--all',), expected=expected)
    path.write_bytes(b'')
    run(path, 'x', expected=[], code=1)
    run(path, '^$', '--regex', expected=[0])
    # A sparse 200 GiB file verifies full-size mmap and 64-bit positions without
    # requiring 200 GiB of disk blocks or scanning the entire sparse range.
    big = pathlib.Path(tmp) / 'large'
    offset = 200 * 1024**3 - 32
    with big.open('wb') as f:
        f.seek(offset)
        f.write(b'needle\n')
    run(big, 'needle', options=('--offset', str(offset - 8)), expected=[offset])
    # Real PTY: literal reverse search and regex mode, then restore termios.
    path.write_bytes(b'first needle\nsecond needle\nlast\n')
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    child = subprocess.Popen([EXE, *SIMD_FLAGS, '--literal', str(path)], stdin=slave, stdout=slave,
                             stderr=slave, env=dict(os.environ, LESS=''))
    def until(token):
        out = b''
        deadline = time.monotonic() + 5
        while token not in out and time.monotonic() < deadline:
            if select.select([master], [], [], .1)[0]:
                out += os.read(master, 65536)
        assert token in out, (token, out)
    until(b'fastless')
    os.write(master, b'/needle\r')
    until(b'Match at byte 6')
    os.write(master, b'n')
    until(b'Match at byte 20')
    os.write(master, b'N')
    until(b'Match at byte 6')
    os.write(master, b'r/second.*needle\r')
    until(b'Match at byte 13')
    os.write(master, b'q')
    assert child.wait(timeout=5) == 0
    assert termios.tcgetattr(slave) == original
    os.close(master)
    os.close(slave)
    checks += 1
    # less-style navigation keys on a 6-row terminal: 5 text rows of 8-byte lines.
    path.write_bytes(b''.join(b'line%03d\n' % i for i in range(100)))
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 6, 120, 0, 0))
    original = termios.tcgetattr(slave)
    child = subprocess.Popen([EXE, *SIMD_FLAGS, str(path)], stdin=slave, stdout=slave,
                             stderr=slave, env=dict(os.environ, LESS=''))
    until(b'byte 0/800')
    last_page = 95 * 8
    for keys, top in [(b'\x1b>', last_page), (b'\x1b<', 0), (b'>', last_page), (b'<', 0),
                      (b'\x1b[B', 8), (b'\x1b[A', 0), (b'\x1bOB', 8), (b'y', 0), (b'e', 8),
                      (b'\x10', 0), (b'\x0e', 8), (b'\x0b', 0), (b'\x05', 8), (b'\x19', 0),
                      (b'\x1b[6~', 40), (b'\x1b[5~', 0), (b'\x1b ', 40), (b'\x1bv', 0),
                      (b'\x06', 40), (b'\x02', 0), (b'z', 40), (b'\x16', 80),
                      (b'd', 96), (b'u', 80), (b'\x04', 96), (b'\x15', 80),
                      (b'\x1b[F', last_page), (b'\x1b[H', 0), (b'\x1b[4~', last_page),
                      (b'\x1b[1~', 0), (b'\x1bOF', last_page), (b'\x1bOH', 0),
                      (b'\x1b[1;5B', 8), (b'\x1bxj', 16), (b'\x1b[99~j', 24)]:
        os.write(master, keys)
        until(b'byte %d/800' % top)
        checks += 1
    os.write(master, b'Q')
    assert child.wait(timeout=5) == 0
    assert termios.tcgetattr(slave) == original
    os.close(master)
    os.close(slave)
    # Differential reverse-search checks exercise skip-table shifts and overlaps.
    buf = b'abcabca aaaa aaab abcABC abcabca abca aaaa aaa aaabc'
    path.write_bytes(buf)
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    child = subprocess.Popen([EXE, *SIMD_FLAGS, '--literal', '-I', str(path)], stdin=slave,
                             stdout=slave, stderr=slave, env=dict(os.environ, LESS=''))
    until(b'fastless')
    for pat in ('abca', 'aaa', 'abc', 'a', 'ABC'):
        hits = [i for i in range(len(buf)) if buf.lower().startswith(pat.lower().encode(), i)]
        os.write(master, b'g/' + pat.encode() + b'\r')
        until(f'Match at byte {hits[0]}'.encode())
        for hit in hits[1:]:
            os.write(master, b'n')
            until(f'Match at byte {hit}'.encode())
        for hit in reversed(hits[:-1]):
            os.write(master, b'N')
            until(f'Match at byte {hit}'.encode())
        checks += 1
    child.send_signal(signal.SIGTERM)
    assert child.wait(timeout=5) == 128 + signal.SIGTERM
    assert termios.tcgetattr(slave) == original
    os.close(master)
    os.close(slave)
    checks += 1
    # Default mode is regex, and the pager rejects redirected stdin/stdout.
    path.write_bytes(b'abc123\n')
    e = dict(os.environ, LESS='')
    p = subprocess.run([EXE, *SIMD_FLAGS, '--find', '^abc[0-9]+$', str(path)],
                       capture_output=True, env=e, timeout=5)
    assert p.returncode == 0 and p.stdout == b'0\n'
    p = subprocess.run([EXE, *SIMD_FLAGS, str(path)], capture_output=True, env=e, timeout=5)
    assert p.returncode == 2 and b'terminal' in p.stderr
    checks += 2
    # Inspect actual terminal output, including reset boundaries and redraws.
    def highlight_case(payload, pattern, mode, expected, options=(), redraw=None,
                       env=None):
        global checks
        path.write_bytes(payload)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 6, 120, 0, 0))
        original = termios.tcgetattr(slave)
        e = dict(os.environ, LESS='')
        if env:
            e.update(env)
        child = subprocess.Popen([EXE, *SIMD_FLAGS, mode, *options, str(path)], stdin=slave,
                                 stdout=slave, stderr=slave, env=e)
        def frame(after=b''):
            out = b''
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    out += os.read(master, 65536)
                start = out.rfind(b'\x1b[H')
                status = out.find(b'\x1b[7m\x1b[2K', start)
                if (start >= 0 and status >= 0 and after in out[:start]
                        and b'\x1b[0m' in out[status:]):
                    return out[start:status].replace(b'\r\r\n', b'\r\n')
            raise AssertionError(('Incomplete terminal frame', out))
        try:
            initial = frame()
            assert b'\x1b[7m' not in initial
            os.write(master, b'/' + pattern.encode() + b'\r')
            rendered = frame(b'Searching...')
            for fragment in expected:
                assert fragment in rendered, (pattern, fragment, rendered)
            if redraw:
                os.write(master, redraw[0])
                rendered = frame()
                assert redraw[1] in rendered, (redraw, rendered)
            os.write(master, b'q')
            assert child.wait(timeout=5) == 0
            assert termios.tcgetattr(slave) == original
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            os.close(master)
            os.close(slave)
        checks += 1
    on, off = b'\x1b[7m', b'\x1b[0m'
    highlight_case(b'one needle two needle!\nnext needle\n', 'needle', '--literal',
                   [b'one ' + on + b'needle' + off + b' two ' + on + b'needle' + off + b'!',
                    b'next ' + on + b'needle' + off],
                   redraw=(b'j', b'next ' + on + b'needle' + off))
    highlight_case(b'ababa!\n', 'aba', '--literal', [on + b'ababa' + off + b'!'])
    highlight_case(b'Alpha ALPHA!\n', 'alpha', '--literal',
                   [on + b'Alpha' + off + b' ' + on + b'ALPHA' + off], env={'LESS': '-i'})
    highlight_case(b'error12 ok error345!\n', 'error[0-9]+', '--regex',
                   [on + b'error12' + off + b' ok ' + on + b'error345' + off + b'!'])
    highlight_case(b'ababa!\n', 'aba', '--regex', [on + b'ababa' + off + b'!'])
    highlight_case(b'aaab!\n', 'a*', '--regex', [on + b'aaa' + off + b'b!'])
    highlight_case(b'plain\n', '^', '--regex', [b'\x1b[2Kplain\r\n'])
    highlight_case(b'x' * 150 + b'\n', '^x{150}$', '--regex',
                   [on + b'x' * 119 + off + b'\r\n'])
    highlight_case(b'a\tb!\n', 'a.b', '--regex', [on + b'a       b' + off + b'!'])
    highlight_case(b'a\x1bb!\n', 'a.b', '--regex', [on + b'a.b' + off + b'!'])
    highlight_case(b'has needle here\nplain\n', '*needle*', '--glob',
                   [on + b'has needle here' + off + b'\r\n', b'\x1b[2Kplain\r\n'])
print(f'{checks} integration checks passed')
