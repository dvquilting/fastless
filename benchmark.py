#!/usr/bin/env python3
"""Reproducible warm-cache search benchmark; no third-party dependencies."""
import argparse
import os
import pathlib
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--mib', type=int, default=256)
parser.add_argument('--runs', type=int, default=5)
parser.add_argument('--compare', action='store_true', help='compare custom SIMD off/auto')
parser.add_argument('--baseline', help='also compare an older executable')
parser.add_argument('--exe', default=str(pathlib.Path(__file__).with_name('fastless').resolve()))
args = parser.parse_args()
if args.mib < 1 or args.runs < 1:
    parser.error('--mib and --runs must be positive')
exe = pathlib.Path(args.exe).resolve()
# 1 KiB lines; no pattern until the last line.
prefix = b'2026-10-12 INFO ordinary record '
line = prefix + b'x' * (1023 - len(prefix)) + b'\n'
assert len(line) == 1024
block = line * 1024
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / 'bench.log'
    with path.open('wb') as f:
        for _ in range(args.mib):
            f.write(block)
        f.write(b'unique-needle-123\n')
    env = dict(os.environ, LESS='')
    for mode, extra, pattern in (
        ('literal', [], 'unique-needle-123'),
        ('literal -I', ['-I'], 'UNIQUE-NEEDLE-123'),
        ('regex plain', [], 'unique-needle-123'),
        ('regex prefix', [], 'unique-needle-[0-9]+'),
        ('glob', [], '*unique-needle-123*'),
    ):
        variants = [(str(exe), [], 'auto')]
        if args.compare:
            variants = [(str(exe), ['--simd', 'off'], 'off'),
                        (str(exe), ['--simd', 'auto'], 'auto')]
        if args.baseline:
            variants.insert(0, (str(pathlib.Path(args.baseline).resolve()), [], 'before'))
        measurements = []
        # Interleave variants to reduce bias from cache/frequency changes.
        for binary, flags, label in variants:
            subprocess.run([binary, '--' + mode.split()[0], *extra, *flags,
                            '--find', pattern, str(path)], env=env, capture_output=True, check=True)
        samples = {label: [] for _, _, label in variants}
        for iteration in range(args.runs):
            ordered = variants if iteration % 2 == 0 else list(reversed(variants))
            for binary, flags, label in ordered:
                start = time.perf_counter()
                result = subprocess.run([binary, '--' + mode.split()[0], *extra, *flags,
                                         '--find', pattern, str(path)], env=env,
                                        capture_output=True, check=True)
                assert result.stdout.strip() == str(args.mib * 1024**2).encode()
                samples[label].append(time.perf_counter() - start)
        for _, _, label in variants:
            seconds = statistics.median(samples[label])
            measurements.append(f'{label}: {args.mib / 1024 / seconds:.2f} GiB/s')
        print(f'{mode:13s} ' + ' | '.join(measurements))
