#!/usr/bin/env python3
"""C vs Rust benchmark: same algorithms/libraries, verified offsets, warm cache.
Use --compressed to also measure full decode + mmap + literal search.
"""
import argparse
import json
import os
import pathlib
import random
import shutil
import statistics
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--mib', type=int, default=256)
parser.add_argument('--runs', type=int, default=7)
parser.add_argument('--c', default=str(ROOT / 'fastless'))
parser.add_argument('--rust', default=str(ROOT / 'rust/target/release/fastless-rs'))
parser.add_argument('--tmpdir', default=None)
parser.add_argument('--compressed', action='store_true')
parser.add_argument('--json', default=None, help='save full results and sample timings')
args = parser.parse_args()
if args.mib < 1 or args.runs < 1:
    parser.error('--mib and --runs must be positive')
binaries = {'C': pathlib.Path(args.c).resolve(), 'Rust': pathlib.Path(args.rust).resolve()}
for path in binaries.values():
    if not path.is_file():
        parser.error(f'Build the executable first: {path}')
if args.compressed:
    for cmd in ('gzip', 'zstd', 'xz'):
        if not shutil.which(cmd):
            parser.error(f'{cmd} is required to prepare compressed fixtures')
MIB = 1024**2
marker = b'unique-needle-123\n'
prefix = b'2026-10-12 INFO ordinary record '
repetitive = (prefix + b'x' * (1023 - len(prefix)) + b'\n') * 1024
rng = random.Random(42)
rows = []
row_bytes = 0
while row_bytes < MIB:
    row = (f'2026-10-12T12:{rng.randrange(60):02d}:00 INFO '
           f'service={rng.choice(["api", "cache", "worker", "database"])} '
           f'request={rng.randrange(10**9)} elapsed={rng.randrange(10000)} '
           f'status={rng.choice([200, 201, 204, 302])} '
           f'path=/api/{rng.choice(["records", "users", "orders", "events"])} '
           f'message=ordinary success value={rng.randrange(10**9)}\n').encode()
    rows.append(row)
    row_bytes += len(row)
mixed = b''.join(rows)[:MIB - 1] + b'\n'
assert len(repetitive) == len(mixed) == MIB
operations = [
    ('literal', ['--literal'], 'unique-needle-123'),
    ('literal -I', ['--literal', '-I'], 'UNIQUE-NEEDLE-123'),
    ('regex plain', ['--regex'], 'unique-needle-123'),
    ('regex prefix', ['--regex'], 'unique-needle-[0-9]+'),
    ('regex general', ['--regex'], '(unique)-needle-[0-9]+'),
    ('glob simple', ['--glob'], '*unique-needle-123*'),
    ('glob general', ['--glob'], '*unique?needle*123*'),
]
results = []
with tempfile.TemporaryDirectory(dir=args.tmpdir) as tmp:
    root = pathlib.Path(tmp)
    spool = root / 'spool'
    spool.mkdir()
    env = dict(os.environ, LESS='', TMPDIR=str(spool))
    variants = [('C', 'auto'), ('Rust', 'auto'), ('C', 'off'), ('Rust', 'off')]
    def measure(dataset, operation, source, flags, pattern, chosen=variants):
        expected = str(args.mib * MIB).encode() + b'\n'
        samples = {f'{lang}/{simd}': [] for lang, simd in chosen}
        commands = {f'{lang}/{simd}': [str(binaries[lang]), *flags, '--simd', simd,
                                      '--find', pattern, str(source)] for lang, simd in chosen}
        # Warm and validate every executable before timing.
        for command in commands.values():
            p = subprocess.run(command, capture_output=True, env=env, check=True)
            assert p.stdout == expected, (command, p.stdout, expected)
        for iteration in range(args.runs):
            order = list(commands)
            # Deterministic shuffled order avoids always favoring one language.
            random.Random(1000 + iteration).shuffle(order)
            for variant in order:
                start = time.perf_counter()
                p = subprocess.run(commands[variant], capture_output=True, env=env, check=True)
                seconds = time.perf_counter() - start
                assert p.stdout == expected, (commands[variant], p.stdout, expected)
                samples[variant].append(seconds)
        medians = {key: statistics.median(values) for key, values in samples.items()}
        speeds = {key: args.mib / 1024 / value for key, value in medians.items()}
        ratio = medians['C/auto'] / medians['Rust/auto']
        results.append({'dataset': dataset, 'operation': operation, 'gib_per_second': speeds,
                        'rust_speed_ratio': ratio, 'median_seconds': medians,
                        'samples_seconds': samples})
        fields = [f'{dataset}/{operation}', f'{speeds["C/auto"]:.2f}', f'{speeds["Rust/auto"]:.2f}',
                  f'{ratio:.2f}x', f'{speeds["C/off"]:.2f}' if 'C/off' in speeds else '—',
                  f'{speeds["Rust/off"]:.2f}' if 'Rust/off' in speeds else '—']
        print('| ' + ' | '.join(fields) + ' |', flush=True)
    print(f'{args.mib} MiB fixtures; median of {args.runs} interleaved warm-cache runs; GiB/s.', flush=True)
    print('| Fixture/search | C auto | Rust auto | Rust/C speed | C off | Rust off |')
    print('| --- | ---: | ---: | ---: | ---: | ---: |')
    for name, block in [('log', repetitive), ('mixed', mixed)]:
        source = root / 'fixture.log'
        with source.open('wb') as f:
            for _ in range(args.mib):
                f.write(block)
            f.write(marker)
        for operation, flags, pattern in operations:
            measure(name, operation, source, flags, pattern)
        if args.compressed and name == 'mixed':
            threads = str(min(os.cpu_count() or 1, 32))
            for codec, cmd in [('gzip', ['gzip', '-1', '-c']),
                               ('zstd', ['zstd', '-1', '-q', '-c']),
                               ('xz', ['xz', '-1', '-T' + threads, '--block-size=4MiB', '-c'])]:
                compressed = root / ('fixture.' + codec)
                with compressed.open('wb') as f:
                    subprocess.run([*cmd, str(source)], stdout=f, check=True)
                measure('mixed compressed', codec + ' decode+literal', compressed,
                        ['--literal', '--threads', threads], 'unique-needle-123', variants[:2])
                compressed.unlink()
    assert not list(spool.iterdir())
if args.json:
    report = {'mib': args.mib, 'runs': args.runs, 'cache': 'warm',
              'c_executable': str(binaries['C']), 'rust_executable': str(binaries['Rust']),
              'results': results}
    pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + '\n')
