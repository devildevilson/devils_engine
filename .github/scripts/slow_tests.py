"""Publish the slowest ctest tests of a log as a notice annotation.

Usage: slow_tests.py TEST_LOG [COUNT]. Annotations are readable without signing in, so the test
timings of a runner stay visible even when the job succeeds or is cancelled.
"""
import re
import sys
from pathlib import Path

# "12/802 Test #34: name with spaces ..........   Passed    1.23 sec"
RESULT = re.compile(r"Test\s+#\d+: (.+?) \.+\s+(\S.*?)\s+([\d.]+) sec")

path = Path(sys.argv[1])
count = int(sys.argv[2]) if len(sys.argv) > 2 else 25
if not path.is_file():
    print(f"::notice title=slowest tests::{path} is missing")
    sys.exit(0)

results = []
for line in path.read_text(errors="replace").splitlines():
    match = RESULT.search(line)
    if match:
        results.append((float(match.group(3)), match.group(1), match.group(2).strip("* ")))
results.sort(reverse=True)
total = sum(seconds for seconds, _, _ in results)
lines = [f"{seconds:8.1f} s  {status:<10} {name}" for seconds, name, status in results[:count]]
message = f"{len(results)} tests finished, {total:.0f} s in total%0A" + "%0A".join(lines)
print(f"::notice title=slowest tests::{message}")
