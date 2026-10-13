#!/usr/bin/env python3
"""Warm-cache end-to-end decode + mmap search; fixture setup is not timed.
Requires gzip, zstd, and xz executables to generate fixtures and compare them.
"""
import argparse
import os
import pathlib
import shutil
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--mib', type=int, default=256)
parser.add_argument('--runs', type=int, default=3)
parser.add_argument('--tmpdir', default=None)
args = parser.parse_args()
if args.mib < 1 or args.runs < 1:
    parser.error('--mib and --runs must be positive')
for command in ('gzip', 'zstd', 'xz'):
    if not shutil.which(command):
        parser.error(f'{command} executable is required for this benchmark')
exe = pathlib.Path(os.environ.get('FASTLESS_BENCH_EXE',
                                 pathlib.Path(__file__).with_name('fastless'))).resolve()
prefix = b'2026-10-12 INFO ordinary record '
block = (prefix + b'x' * (1023 - len(prefix)) + b'\n') * 1024
threads = str(min(os.cpu_count() or 1, 32))
with tempfile.TemporaryDirectory(dir=args.tmpdir) as tmp:
    root = pathlib.Path(tmp)
    source = root / 'bench.log'
    spool = root / 'spool'
    spool.mkdir()
    decoded = root / 'decoded.log'
    with source.open('wb') as f:
        for _ in range(args.mib):
            f.write(block)
        f.write(b'unique-needle-123\n')
    env = dict(os.environ, LESS='', TMPDIR=str(spool))
    search = ['--literal', '--find', 'unique-needle-123']
    expected = str(args.mib * 1024**2).encode() + b'\n'
    print(f'{args.mib} MiB log fixture; median of {args.runs} warm-cache runs; output MiB/s')
    for codec, encoder, decoder in (
        ('gzip', ['gzip', '-1', '-c'], ['gzip', '-d', '-c']),
        ('zstd', ['zstd', '-1', '-q', '-c'], ['zstd', '-q', '-d', '-c']),
        ('xz', ['xz', '-1', '-T' + threads, '--block-size=4MiB', '-c'],
         ['xz', '-d', '-T' + threads, '-c']),
    ):
        compressed = root / ('bench.' + codec)
        with compressed.open('wb') as f:
            subprocess.run([*encoder, str(source)], stdout=f, check=True)
        measurements = []
        for direct in (True, False):
            times = []
            for _ in range(args.runs):
                start = time.perf_counter()
                if direct:
                    p = subprocess.run([str(exe), *search, '--threads', threads,
                                        str(compressed)], capture_output=True, env=env, check=True)
                else:
                    with decoded.open('wb') as f:
                        subprocess.run([*decoder, str(compressed)], stdout=f, env=env, check=True)
                    p = subprocess.run([str(exe), *search, str(decoded)], capture_output=True,
                                       env=env, check=True)
                assert p.stdout == expected, p
                if decoded.exists():
                    decoded.unlink()
                times.append(time.perf_counter() - start)
            measurements.append(statistics.median(times))
        native, cli = measurements
        print(f'{codec:5s}: fastless {native:.3f}s ({args.mib/native:.0f} MiB/s); '
              f'CLI decode + fastless {cli:.3f}s ({args.mib/cli:.0f} MiB/s)')
