#!/usr/bin/env python3
"""Codec integration tests. zstd fixtures use the linked native library via ctypes."""
import bz2
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
import zlib

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

def compress_zstd(data, level=3, content_size=True):
    ctx = zstd.ZSTD_createCCtx()
    assert ctx
    try:
        # ZSTD_c_compressionLevel, ZSTD_c_checksumFlag and
        # ZSTD_c_contentSizeFlag, public enums from zstd.h.
        assert not zstd.ZSTD_isError(zstd.ZSTD_CCtx_setParameter(ctx, 100, level))
        assert not zstd.ZSTD_isError(zstd.ZSTD_CCtx_setParameter(ctx, 201, 1))
        assert not zstd.ZSTD_isError(zstd.ZSTD_CCtx_setParameter(ctx, 200, int(content_size)))
        dest = ctypes.create_string_buffer(zstd.ZSTD_compressBound(len(data)))
        n = zstd.ZSTD_compress2(ctx, dest, len(dest), data, len(data))
        assert not zstd.ZSTD_isError(n)
        return dest.raw[:n]
    finally:
        zstd.ZSTD_freeCCtx(ctx)

codecs = {'gzip': gzip.compress, 'zstd': compress_zstd, 'xz': lzma.compress, 'bzip2': bz2.compress}
SIMD_FLAGS = (["--simd", os.environ["FASTLESS_TEST_SIMD"]]
              if "FASTLESS_TEST_SIMD" in os.environ else [])
checks = 0


def bzip2_blocks(root):
    """Multi-block bzip2, decoded block by block on several threads; the tiny
    test build also misses and misplaces some block starts."""
    global checks
    source = root / 'blocks.bz2'
    rng = random.Random(11)
    text = b''.join(b'%07d %s\n' % (i, bytes(rng.choice(b'abcdefgh ') for _ in range(rng.randint(0, 90))))
                    for i in range(120000))
    runs = b''.join(bytes([rng.randrange(256)]) * rng.randint(1, 3000) for _ in range(20000))
    blobs = {
        'text, 100k blocks': bz2.compress(text, 1),
        'text, 900k blocks': bz2.compress(text, 9),
        'streams': bz2.compress(text[:5000], 2) + bz2.compress(b'') + bz2.compress(text[5000:], 9),
        'long runs': bz2.compress(runs, 9),
        'zeros': bz2.compress(bytes(100_000_000), 9),  # 45 MB from each block
    }
    env = dict(os.environ, LESS='', LESSOPEN='')
    for name, blob in blobs.items():
        source.write_bytes(blob)
        want = bz2.decompress(blob)
        for threads in ('1', '2', '16'):
            p = subprocess.run([EXE, '--threads', threads, str(source)], capture_output=True, env=env, timeout=120)
            assert p.returncode == 0 and p.stdout == want, (name, threads, p.returncode, len(p.stdout), p.stderr)
            checks += 1
    blob = blobs['text, 900k blocks']
    damaged = bytearray(blob)
    damaged[len(blob) // 2] ^= 4
    for bad, message in ((blob[:len(blob) // 2], b'Truncated'), (blob[:-1], b'Truncated'),
                         (bytes(damaged), b'bzip2'), (blob + b'junk', b'after the last stream')):
        source.write_bytes(bad)
        p = subprocess.run([EXE, str(source)], capture_output=True, env=env, timeout=60)
        assert p.returncode == 2 and not p.stdout and message in p.stderr, (message, p)
        checks += 1
    source.write_bytes(blob + bytes(9))
    p = subprocess.run([EXE, str(source)], capture_output=True, env=env, timeout=60)
    assert p.returncode == 0 and p.stdout == text, p.stderr
    checks += 1
    # Ctrl-C part-way reports an interruption, not damage, also when the main
    # thread is decoding a block itself (every third one in the tiny build).
    source.write_bytes(bz2.compress(text * 4, 1))
    for delay in (0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35):
        child = subprocess.Popen([EXE, '--threads', '1', '--find', 'absent', str(source)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        time.sleep(delay)
        child.send_signal(signal.SIGINT)
        err = child.communicate(timeout=30)[1]
        assert (child.returncode, err) in ((1, b''), (130, b'bzip2: Decompression interrupted\n')), \
            (delay, child.returncode, err)
        checks += 1


def gzip_member(data, level=6, flush=None, every=0, rng=None, name=None, extra=None,
                comment=None, header_crc=False):
    """One gzip member, optionally flushed every 1..`every` bytes and with
    optional header fields."""
    flags = (4 if extra is not None else 0) | (8 if name is not None else 0) | \
        (16 if comment is not None else 0) | (2 if header_crc else 0)
    header = b'\x1f\x8b\x08' + bytes([flags]) + b'\0\0\0\0\0\xff'
    if extra is not None:
        header += struct.pack('<H', len(extra)) + extra
    for field in (name, comment):
        if field is not None:
            header += field + b'\0'
    if header_crc:
        header += struct.pack('<H', zlib.crc32(header) & 0xffff)
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    body = []
    pos = 0
    while pos < len(data):
        step = rng.randint(1, every) if every else len(data)
        body.append(c.compress(data[pos:pos + step]))
        if flush is not None:
            body.append(c.flush(flush))
        pos += step
    body.append(c.flush())
    return header + b''.join(body) + struct.pack('<II', zlib.crc32(data), len(data) & 0xffffffff)


def gzip_chunks(root):
    """gzip decoded on several threads from guessed block starts; the tiny
    test build makes chunks of a few KiB, most without a start of their own."""
    global checks
    source = root / 'chunks.gz'
    rng = random.Random(13)
    vocabulary = [bytes(rng.choice(b'abcdefghijklmnopqrstuvwxyz') for _ in range(rng.randint(2, 9)))
                  for _ in range(3000)]
    words = b''.join(b'%06d %s\n' % (i, b' '.join(rng.choice(vocabulary) for _ in range(rng.randint(1, 12))))
                     for i in range(40000))
    runs = b''.join(bytes([rng.randrange(256)]) * rng.randint(1, 40) for _ in range(30000))
    noise = rng.randbytes(300000)
    members = []
    for i in range(40):
        piece = words[rng.randrange(len(words)):][:rng.choice((0, 10, 3000, 70000))]
        members.append((piece, gzip_member(piece, rng.choice((1, 6, 9)), zlib.Z_BLOCK if i % 3 else None,
                                           rng.randint(100, 9000), rng, name=b'm%d' % i if i % 2 else None,
                                           extra=b'x' * (i * 37 % 300) if i % 5 == 1 else None,
                                           comment=b'c' if i % 7 == 2 else None, header_crc=i % 4 == 3)))
    blobs = {
        'level 6': (words, gzip.compress(words, 6)),
        'level 1': (words, gzip.compress(words, 1)),
        'unaligned blocks': (words, gzip_member(words, 9, zlib.Z_BLOCK, 3000, rng)),
        'sync flushes': (words, gzip_member(words, 6, zlib.Z_SYNC_FLUSH, 20000, rng)),
        'full flushes': (words, gzip_member(words, 6, zlib.Z_FULL_FLUSH, 10000, rng)),
        'fixed blocks': (words[:300000], gzip_member(words[:300000], 6, zlib.Z_BLOCK, 60, rng)),
        'members': (b''.join(m[0] for m in members), b''.join(m[1] for m in members)),
        'stored': (noise, gzip.compress(noise, 0)),
        'incompressible': (noise, gzip.compress(noise, 6)),
        'runs': (runs, gzip.compress(runs, 9)),
        'zeros': (bytes(30_000_000), gzip.compress(bytes(30_000_000), 6)),
        'large': (words * 6, gzip_member(words * 6, 6, zlib.Z_BLOCK, 200000, rng)),
    }
    blobs['padded'] = (blobs['members'][0], blobs['members'][1] + bytes(100))
    env = dict(os.environ, LESS='', LESSOPEN='')
    for name, (want, blob) in blobs.items():
        assert gzip.decompress(blob[:-100] if name == 'padded' else blob) == want, name
        source.write_bytes(blob)
        for threads in ('1', '4', '16'):
            p = subprocess.run([EXE, '--threads', threads, str(source)], capture_output=True, env=env, timeout=120)
            assert p.returncode == 0 and p.stdout == want, (name, threads, p.returncode, len(p.stdout), p.stderr)
            checks += 1
    want, blob = blobs['unaligned blocks']
    bad_crc = bytearray(blob)
    bad_crc[-8] ^= 1
    bad_size = bytearray(blob)
    bad_size[-1] ^= 1
    middle = bytearray(blob)
    middle[len(blob) // 2] ^= 0x10
    second = blobs['members'][1]
    for bad, message in ((blob[:len(blob) // 2], b'Truncated'), (blob[:-1], b'Truncated'), (blob[:-8], b'Truncated'),
                         (bytes(bad_crc), b'checksum'), (bytes(bad_size), b'checksum'), (bytes(middle), b'gzip'),
                         (blob + b'junk', b'gzip'), (blob + b'\x1f\x8b\x09' + bytes(20), b'gzip'),
                         (second[:len(second) * 2 // 3], b'Truncated')):
        source.write_bytes(bad)
        for threads in ('4', '16'):
            p = subprocess.run([EXE, '--threads', threads, str(source)], capture_output=True, env=env, timeout=60)
            assert p.returncode == 2 and not p.stdout and message in p.stderr, (message, threads, p)
            checks += 1
    # Output that outgrows memory part-way moves to a file, with pieces
    # already placed in memory by other threads.
    want, blob = blobs['large']
    source.write_bytes(blob)
    for limit in ('0', '0.001', '0.003'):
        p = subprocess.run([EXE, '--threads', '16', '--max-mem', limit, '--tmpdir', str(root), str(source)],
                           capture_output=True, env=env, timeout=120)
        assert p.returncode == 0 and p.stdout == want, (limit, p.returncode, len(p.stdout), p.stderr)
        checks += 1
    # Ctrl-C part-way reports an interruption, not damage.
    source.write_bytes(gzip.compress(words * 20, 6))
    for delay in (0.02, 0.05, 0.1, 0.15, 0.2, 0.3):
        child = subprocess.Popen([EXE, '--threads', '4', '--find', 'absent', str(source)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        time.sleep(delay)
        child.send_signal(signal.SIGINT)
        err = child.communicate(timeout=30)[1]
        assert (child.returncode, err) in ((1, b''), (130, b'gzip: Decompression interrupted\n'),
                                           (130, b'Search interrupted\n')), (delay, child.returncode, err)
        checks += 1


def zstd_frames(root):
    """zstd input of several frames, decoded on several threads; the tiny
    test build puts one or two frames in each run."""
    global checks
    source = root / 'frames.zst'
    rng = random.Random(17)
    lines = b''.join(b'%06d %s\n' % (i, bytes(rng.choice(b'abcdefgh ') for _ in range(rng.randint(0, 90))))
                     for i in range(60000))
    parts = [lines[rng.randrange(len(lines)):][:rng.choice((0, 1, 500, 40000, 300000))] for _ in range(60)]
    skip = struct.pack('<II', 0x184d2a5f, 5) + b'12345'
    blobs = {
        # As pzstd writes them: no content sizes, each after a skippable frame.
        'no sizes': (b''.join(parts), b''.join(skip + compress_zstd(p, rng.choice((1, 3, 9)), False) for p in parts)),
        'sizes': (b''.join(parts), b''.join(compress_zstd(p, rng.choice((1, 3, 19))) for p in parts)),
        'large frames': (lines * 4, b''.join(compress_zstd(lines, 3, i % 2 == 0) for i in range(4))),
        'one frame': (lines, compress_zstd(lines)),
    }
    env = dict(os.environ, LESS='', LESSOPEN='')
    for name, (want, blob) in blobs.items():
        source.write_bytes(blob)
        for threads in ('1', '2', '16'):
            p = subprocess.run([EXE, '--threads', threads, str(source)], capture_output=True, env=env, timeout=120)
            assert p.returncode == 0 and p.stdout == want, (name, threads, p.returncode, len(p.stdout), p.stderr)
            checks += 1
    want, blob = blobs['large frames']
    source.write_bytes(blob)
    for limit in ('0', '0.002'):
        p = subprocess.run([EXE, '--threads', '16', '--max-mem', limit, '--tmpdir', str(root), str(source)],
                           capture_output=True, env=env, timeout=120)
        assert p.returncode == 0 and p.stdout == want, (limit, p.returncode, len(p.stdout), p.stderr)
        checks += 1
    want, blob = blobs['no sizes']
    damaged = bytearray(blob)
    damaged[len(blob) // 2] ^= 0x40
    for bad in (blob[:-3], blob[:len(blob) // 2], bytes(damaged), blob + b'junk', blob + bytes(8)):
        source.write_bytes(bad)
        for threads in ('2', '16'):
            p = subprocess.run([EXE, '--threads', threads, str(source)], capture_output=True, env=env, timeout=60)
            assert p.returncode == 2 and not p.stdout and b'zstd: ' in p.stderr, (threads, p)
            checks += 1


only = os.environ.get('FASTLESS_TEST_ONLY')
if only in ('bzip2', 'gzip', 'zstd', 'threaded'):
    with tempfile.TemporaryDirectory() as tmp:
        if only in ('bzip2', 'threaded'):
            bzip2_blocks(pathlib.Path(tmp))
        if only in ('gzip', 'threaded'):
            gzip_chunks(pathlib.Path(tmp))
        if only in ('zstd', 'threaded'):
            zstd_frames(pathlib.Path(tmp))
    print(f'{checks} threaded decoding checks passed')
    raise SystemExit(0)
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
    missing = str(root / 'missing')
    # Output in memory (the default), always in a file, and spilling to a file
    # after 107374 bytes (0.0001 GiB, rounded down).
    for storage in ((), ('--max-mem', '0'), ('--max-mem', '0.0001')):
        file_only = storage == ('--max-mem', '0')
        for name, compress in codecs.items():
            def check(blob, options=(), **kw):
                return run(blob, options=(*storage, *options), **kw)
            blob = compress(payload)
            check(blob, expected=[6])
            check(blob, options=('--all',), expected=[6, 33])
            check(blob, options=('--all',), expected=[6, 20, 33], env={'LESS': '-i'})
            check(blob, options=('--offset', '7'), expected=[33])
            check(blob, pattern='^third .*$', mode='--regex', expected=[27])
            check(blob, pattern='*needle*', mode='--glob', options=('--all',), expected=[0, 27])
            check(blob, options=('--compression', name), expected=[6])
            check(blob, temp_option=False, expected=[6])  # TMPDIR fallback
            # Only output that reaches a file needs the temporary directory.
            if file_only:
                check(blob, options=('--tmpdir', missing), expected=[], code=2)
            else:
                check(blob, options=('--tmpdir', missing), expected=[6])
            check(blob, options=('--compression', 'gzip' if name != 'gzip' else 'xz'), code=2)
            check(compress(b''), code=1, expected=[])
            check(compress(b''), options=('--tmpdir', missing), code=1, expected=[])
            check(compress(b'') + compress(payload) + compress(b''), expected=[6])
            check(compress(b'first needle\n') + compress(b'third needle\n'),
                  options=('--all',), expected=[6, 19])
            for truncated in (blob[:-1], blob[:len(blob) // 2], blob[:10]):
                check(truncated, code=2)
            damaged = bytearray(blob)
            # bzip2's last byte may be only padding bits.
            damaged[-8 if name == 'gzip' else len(blob) // 2 if name == 'bzip2' else -1] ^= 1
            check(damaged, code=2)
            check(blob + b'trailing junk', code=2)
            check(b'', options=('--compression', name), code=2)
            # >8 MiB of uncompressed bytes exercises output flushing.
            large = b'x' * (9 * 1024**2) + b'needle\n'
            check(compress(large), expected=[9 * 1024**2])
            # >8 MiB of compressed bytes exercises input refills, not just expansion.
            large = random.Random(42).randbytes(9 * 1024**2) + b'needle\n'
            check(compress(large), expected=[large.find(b'needle')])
            # Matches throughout, including one straddling the spill point,
            # verify every byte that moved from memory to the file.
            spread = bytearray(random.Random(7).choice(b'abc\n') for _ in range(300000))
            for at in (0, 50000, 107371, 107374, 200000, len(spread) - 6):
                spread[at:at + 6] = b'needle'
            check(compress(bytes(spread)), options=('--all',),
                  expected=[i for i in range(len(spread)) if spread.startswith(b'needle', i)])
    # The limit is inclusive: output of exactly that size stays in memory.
    for name, compress in codecs.items():
        for limit, size in (('0.0001', 107374), ('0.0078125', 8 * 1024**2)):
            exact = b'x' * (size - 7) + b'needle\n'
            run(compress(exact), options=('--max-mem', limit, '--tmpdir', missing),
                expected=[size - 7])
            run(compress(exact + b'\n'), options=('--max-mem', limit, '--tmpdir', missing), code=2)
            run(compress(exact + b'\n'), options=('--max-mem', limit), expected=[size - 7])
    for value in ('.5', '5.', '00.25', '1000'):
        run(gzip.compress(payload), options=('--max-mem', value), expected=[6])
    for value in ('-1', '', '.', '1.2.3', '1e3', 'inf', 'nan', ' 1', '1 ', '0x10', '1G',
                  '99999999999'):
        run(gzip.compress(payload), options=('--max-mem', value), code=2)
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
    # A file-size limit forces a short write and then a real write error, both
    # when writing decoded output and when moving it out of memory.
    source.write_bytes(gzip.compress(b'x' * 8192 + b'needle'))
    def limit_output():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
    for limit in ('0', '0.000005'):
        p = subprocess.run([EXE, *SIMD_FLAGS, '--literal', '--find', 'needle', '--tmpdir', str(spool),
                            '--max-mem', limit, str(source)],
                           capture_output=True, preexec_fn=limit_output, timeout=10)
        assert p.returncode == 2 and not p.stdout and b'write failed' in p.stderr, p
        assert not list(spool.iterdir())
        checks += 1
    bzip2_blocks(root)
    gzip_chunks(root)
    zstd_frames(root)
    run(bz2.compress(payload) + b'\0' * 16, expected=[6])
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
    # Interrupt a large expansion before raw terminal mode starts, in memory
    # and in a file; temp files vanish.
    source.write_bytes(gzip.compress(b'\0' * (256 * 1024**2), compresslevel=1))
    for storage in ((), ('--max-mem', '0'), ('--max-mem', '0.01')):
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        child = subprocess.Popen([EXE, *SIMD_FLAGS, '--tmpdir', str(spool), *storage,
                                  str(source)],
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
    # Piped input is read on a background thread: copied out as it arrives
    # (cat mode) and searched once whole, in memory, in a file, and spilling
    # part-way; compressed pipes are decoded after they are read.
    piped = b''.join(b'%06d needle %s\n' % (i, b'x' * (i % 50)) for i in range(20000))
    for storage in ((), ('--max-mem', '0'), ('--max-mem', '0.0001')):
        for blob in (piped, gzip.compress(piped), compress_zstd(piped), lzma.compress(piped), bz2.compress(piped)):
            e = dict(os.environ, LESS='', TMPDIR=str(spool))
            base = [EXE, *SIMD_FLAGS, '--tmpdir', str(spool), *storage]
            p = subprocess.run(base, input=blob, capture_output=True, env=e, timeout=30)
            assert p.returncode == 0 and p.stdout == piped, (storage, p.returncode, len(p.stdout), p.stderr)
            p = subprocess.run([*base, '--literal', '--find', '019999 needle'], input=blob,
                               capture_output=True, env=e, timeout=30)
            assert p.returncode == 0 and p.stdout == b'%d\n' % piped.index(b'019999 needle'), (storage, p)
            assert not list(spool.iterdir()), list(spool.iterdir())
            checks += 2
    p = subprocess.run([EXE, *SIMD_FLAGS], input=gzip.compress(piped)[:-9], capture_output=True,
                       env=dict(os.environ, LESS=''), timeout=30)
    assert p.returncode == 2 and not p.stdout and b'gzip' in p.stderr, p
    checks += 1
    # A compressed pipe held in memory counts against --max-mem, so its output
    # gets only the rest: here, too little, so it needs a temporary file.
    noise = random.Random(5).randbytes(64 << 20)
    blob = gzip.compress(noise, compresslevel=1)
    missing = str(root / 'missing')
    for extra in ([], ['--find', 'absent']):
        args = [EXE, *SIMD_FLAGS, '--max-mem', '0.1', *extra]
        p = subprocess.run([*args, '--tmpdir', str(spool)], input=blob, capture_output=True,
                           env=dict(os.environ, LESS=''), timeout=60)
        assert p.returncode == (1 if extra else 0) and (extra or p.stdout == noise), (extra, p.returncode, p.stderr)
        p = subprocess.run([*args, '--tmpdir', missing], input=blob, capture_output=True,
                           env=dict(os.environ, LESS=''), timeout=60)
        assert p.returncode == 2 and b'Cannot create temporary file in ' + missing.encode() in p.stderr, \
            (extra, p.returncode, p.stderr)
        checks += 2
print(f'{checks} compression checks passed')
