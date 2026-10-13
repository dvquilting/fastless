#!/usr/bin/env python3
"""Codec integration tests. zstd fixtures use the linked native library via ctypes."""
import ctypes
import ctypes.util
import gzip
import lzma
import os
import pathlib
import pty
import random
import resource
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time

EXE = os.environ.get('FASTLESS_TEST_EXE', str(pathlib.Path(__file__).with_name('fastless').resolve()))
zstd = ctypes.CDLL(ctypes.util.find_library('zstd'))
zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
zstd.ZSTD_compressBound.restype = ctypes.c_size_t
zstd.ZSTD_createCCtx.restype = ctypes.c_void_p
zstd.ZSTD_freeCCtx.argtypes = [ctypes.c_void_p]
zstd.ZSTD_CCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
zstd.ZSTD_CCtx_setParameter.restype = ctypes.c_size_t
zstd.ZSTD_compress2.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.c_void_p, ctypes.c_size_t]
zstd.ZSTD_compress2.restype = ctypes.c_size_t
zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
zstd.ZSTD_isError.restype = ctypes.c_uint

def compress_zstd(data):
    ctx = zstd.ZSTD_createCCtx()
    assert ctx
    try:
        # ZSTD_c_checksumFlag, a public enum from zstd.h.
        assert not zstd.ZSTD_isError(zstd.ZSTD_CCtx_setParameter(ctx, 201, 1))
        dest = ctypes.create_string_buffer(zstd.ZSTD_compressBound(len(data)))
        n = zstd.ZSTD_compress2(ctx, dest, len(dest), data, len(data))
        assert not zstd.ZSTD_isError(n)
        return dest.raw[:n]
    finally:
        zstd.ZSTD_freeCCtx(ctx)

codecs = {'gzip': gzip.compress, 'zstd': compress_zstd, 'xz': lzma.compress}
SIMD_FLAGS = (["--simd", os.environ["FASTLESS_TEST_SIMD"]]
              if "FASTLESS_TEST_SIMD" in os.environ else [])
checks = 0
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    spool = root / 'spool'
    spool.mkdir()
    source = root / 'no-extension'
    def run(blob, options=(), expected=None, code=0, pattern='needle', mode='--literal',
            temp_option=True, env=None):
        global checks
        source.write_bytes(blob)
        e = dict(os.environ, LESS='', TMPDIR=str(spool))
        if env:
            e.update(env)
        args = [EXE, *SIMD_FLAGS, mode, '--find', pattern]
        if temp_option:
            args += ['--tmpdir', str(spool)]
        args += options
        p = subprocess.run([*args, str(source)], capture_output=True, env=e, timeout=30)
        assert p.returncode == code, (options, p.returncode, p.stdout, p.stderr)
        if expected is not None:
            assert p.stdout == b''.join(str(i).encode() + b'\n' for i in expected), (p.stdout, expected, p.stderr)
        if code == 2:
            assert not p.stdout, p.stdout
            assert p.stderr, 'Expected a useful decompression error'
        assert not list(spool.iterdir()), list(spool.iterdir())
        checks += 1
        return p
    payload = b'first needle\nsecond NEEDLE\nthird needle\n'
    for name, compress in codecs.items():
        blob = compress(payload)
        run(blob, expected=[6])
        run(blob, options=('--all',), expected=[6, 33])
        run(blob, options=('--all',), expected=[6, 20, 33], env={'LESS': '-i'})
        run(blob, options=('--offset', '7'), expected=[33])
        run(blob, pattern='^third .*$', mode='--regex', expected=[27])
        run(blob, pattern='*needle*', mode='--glob', options=('--all',), expected=[0, 27])
        run(blob, options=('--compression', name), expected=[6])
        run(blob, temp_option=False, expected=[6])  # TMPDIR fallback
        run(blob, options=('--tmpdir', str(root / 'missing')), expected=[], code=2)
        run(blob, options=('--compression', 'gzip' if name != 'gzip' else 'xz'), code=2)
        run(compress(b''), code=1, expected=[])
        run(compress(b'') + compress(payload) + compress(b''), expected=[6])
        run(compress(b'first needle\n') + compress(b'third needle\n'),
            options=('--all',), expected=[6, 19])
        for truncated in (blob[:-1], blob[:len(blob) // 2], blob[:10]):
            run(truncated, code=2)
        damaged = bytearray(blob)
        damaged[-8 if name == 'gzip' else -1] ^= 1
        run(damaged, code=2)
        run(blob + b'trailing junk', code=2)
        run(b'', options=('--compression', name), code=2)
        # >8 MiB of uncompressed bytes exercises output flushing.
        large = b'x' * (9 * 1024**2) + b'needle\n'
        run(compress(large), expected=[9 * 1024**2])
        # >8 MiB of compressed bytes exercises input refills, not just expansion.
        large = random.Random(42).randbytes(9 * 1024**2) + b'needle\n'
        run(compress(large), expected=[large.find(b'needle')])
    run(gzip.compress(payload) + b'\0' * 16, expected=[6])
    skip = struct.pack('<II', 0x184d2a50, 3) + b'xyz'
    run(skip + compress_zstd(payload) + skip, expected=[6])
    run(skip, code=1, expected=[])
    run(lzma.compress(payload) + b'\0' * 4 + lzma.compress(payload) + b'\0' * 8,
        options=('--all',), expected=[6, 33, 46, 73])
    # Force raw mode, even with compression magic; offsets then refer to source bytes.
    blob = gzip.compress(payload, compresslevel=0)
    run(blob, options=('--compression', 'none'), expected=[blob.find(b'needle')])
    for value in ('-1', '257', 'no', ''):
        run(gzip.compress(payload), options=('--threads', value), code=2)
    run(gzip.compress(payload), options=('--compression', 'no'), code=2)
    # A file-size limit forces a short write and then a real write error.
    source.write_bytes(gzip.compress(b'x' * 8192 + b'needle'))
    def limit_output():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
    p = subprocess.run([EXE, *SIMD_FLAGS, '--literal', '--find', 'needle', '--tmpdir', str(spool),
                        str(source)], capture_output=True, preexec_fn=limit_output, timeout=10)
    assert p.returncode == 2 and not p.stdout and b'write failed' in p.stderr, p
    assert not list(spool.iterdir())
    checks += 1
    # Multiblock xz fixtures from the threaded encoder, if its CLI is installed.
    import shutil
    if shutil.which('xz'):
        large = b'ordinary record\n' * (2 * 1024**2) + b'needle\n'
        blob = subprocess.run(['xz', '-c', '-T2', '--block-size=1MiB', '-1'],
                              input=large, capture_output=True, check=True).stdout
        for threads in ('1', '2', '0'):
            run(blob, options=('--threads', threads), expected=[len(large) - 7])
    # Pager, highlighting, and terminal cleanup on decompressed input.
    for name, compress in codecs.items():
        source.write_bytes(compress(payload))
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        child = subprocess.Popen([EXE, *SIMD_FLAGS, '--literal', '--tmpdir', str(spool), str(source)],
                                 stdin=slave, stdout=slave, stderr=slave,
                                 env=dict(os.environ, LESS=''))
        def until(token):
            out = b''
            deadline = time.monotonic() + 10
            while token not in out and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    out += os.read(master, 65536)
            assert token in out, (token, out)
            return out
        try:
            until(b'fastless')
            os.write(master, b'/needle\r')
            rendered = until(b'Match at byte 6')
            assert b'\x1b[7mneedle\x1b[0m' in rendered, rendered
            os.write(master, b'n')
            until(b'Match at byte 33')
            os.write(master, b'N')
            until(b'Match at byte 6')
            os.write(master, b'q')
            assert child.wait(timeout=5) == 0
            assert termios.tcgetattr(slave) == original
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            os.close(master)
            os.close(slave)
        assert not list(spool.iterdir())
        checks += 1
    # Interrupt a large expansion before raw terminal mode starts; temp files vanish.
    source.write_bytes(gzip.compress(b'\0' * (256 * 1024**2), compresslevel=1))
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    child = subprocess.Popen([EXE, *SIMD_FLAGS, '--tmpdir', str(spool), str(source)],
                             stdin=slave, stdout=slave, stderr=slave)
    try:
        until(b'Decompressing gzip')
        child.send_signal(signal.SIGINT)
        assert child.wait(timeout=10) == 130
        assert termios.tcgetattr(slave) == original
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        os.close(master)
        os.close(slave)
    assert not list(spool.iterdir())
    checks += 1
print(f'{checks} compression checks passed')
