"""Compare two `frontier_online_server --trace` outputs and name the first divergent tick.

usage: compare_traces.py REFERENCE OTHER [GITHUB_OUTPUT]
Lines that do not start with "trace " (log noise, CRLF) are ignored. On divergence prints an
::error:: annotation with both lines and the differing state sections, writes `diverged=true` and
`tick=N` to GITHUB_OUTPUT when given, and exits 1.
"""
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("trace "):
                rows.append(line)
    return rows


def sections(line):
    return dict(part.split(":", 1) for part in line.split()[3:] if ":" in part)


reference, other = load(sys.argv[1]), load(sys.argv[2])
output = sys.argv[3] if len(sys.argv) > 3 else None
if not reference or len(reference) != len(other):
    print(f"::error title=cross-platform trace::{len(reference)} reference ticks vs {len(other)}")
    sys.exit(1)
for ref, oth in zip(reference, other):
    if ref != oth:
        tick = ref.split()[1]
        a, b = sections(ref), sections(oth)
        differ = [k for k in a if a[k] != b.get(k)]
        print(f"::error title=cross-platform trace::first divergent tick {tick}, sections {', '.join(differ)}"
              f"%0Alinux:   {ref}%0Awindows: {oth}")
        if output:
            with open(output, "a", encoding="utf-8") as f:
                f.write(f"diverged=true\ntick={tick}\n")
        sys.exit(1)
print(f"cross-platform trace: {len(reference)} ticks identical, last {reference[-1].split()[2]}")
if output:
    with open(output, "a", encoding="utf-8") as f:
        f.write("diverged=false\n")
