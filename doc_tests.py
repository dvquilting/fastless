"""Checks that the manual page and the PDF manual's source describe every
option in options.c, and that the manual page has no groff warnings."""
import pathlib
import re
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parent
table = (root / 'options.c').read_text()
names = sorted(set(re.findall(r'^\s*(?:FLAG|INT|FUNC|NOOP|UNSUPPORTED)\(\s*(?:\'[^\']+\'|0),\s*"([^"]+)"',
                              table, re.M)))
assert len(names) > 100, names
documents = {
    'doc/fastless.1': (root / 'doc/fastless.1').read_text().replace('\\-', '-'),
    'doc/manual.tex': (root / 'doc/manual.tex').read_text(),
}
missing = [f'{doc}: --{name}' for doc, text in documents.items() for name in names
           if not re.search('--' + re.escape(name) + '(?![A-Za-z-])', text)]
assert not missing, 'options not documented:\n  ' + '\n  '.join(missing)
checks = len(names) * len(documents)
if shutil.which('groff'):
    r = subprocess.run(['groff', '-man', '-ww', '-z', str(root / 'doc/fastless.1')],
                       capture_output=True, text=True)
    assert r.returncode == 0 and not r.stderr, r.stderr
    checks += 1
print(f'{checks} documentation checks passed')
