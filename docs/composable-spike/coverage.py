#!/usr/bin/env python3
"""Make the table of design sections and the tests that check them.

Each test of the spike has a name "<document>/<section>: <description>",
for example "1/merge: only a layer can move". The script reads the unit
tests (test/unit/v2_*.cpp) and the bats suite (test/integration/
composable.bats), and prints a markdown table.

Usage: python3 docs/composable-spike/coverage.py [repo root]
"""
import collections
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
docs = {"1": "1-composition", "2": "2-oci-layout", "3": "3-repository",
        "4": "4-registry-ops", "5": "5-discovery", "6": "6-migration"}

tests = []
for f in sorted((root / "test" / "unit").glob("v2_*.cpp")):
    text = f.read_text()
    for m in re.finditer(r'TEST_CASE\(\s*"((?:[^"\\]|\\.)*)"\s*(?:"((?:[^"\\]|\\.)*)")?', text):
        name = m.group(1) + (m.group(2) or "")
        tests.append(("unit", name))
bats = root / "test" / "integration" / "composable.bats"
for m in re.finditer(r'^@test "([^"]*)"', bats.read_text(), re.M):
    tests.append(("bats", m.group(1)))

table = collections.defaultdict(lambda: collections.Counter())
for kind, name in tests:
    m = re.match(r"(\d)/([^:]+)(?::|$)", name)
    if not m:
        continue
    table[(m.group(1), m.group(2).strip())][kind] += 1

print(f"| {'document':<16} | {'section':<28} | unit | bats |")
print(f"| {'-'*16} | {'-'*28} | ---- | ---- |")
total = collections.Counter()
for (doc, section), c in sorted(table.items()):
    total.update(c)
    print(f"| {docs.get(doc, doc):<16} | {section:<28} | {c['unit']:>4} | {c['bats']:>4} |")
print(f"| {'total':<16} | {'':<28} | {total['unit']:>4} | {total['bats']:>4} |")
