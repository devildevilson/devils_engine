"""Turn error lines of CI logs into workflow annotations.

Job logs need authentication to download, annotations do not: this makes the reason of a failed
step readable from the public API. Usage: report_errors.py LOG [LOG...]
"""
import re
import sys
from pathlib import Path

PATTERNS = re.compile(
    r"(CMake Error|error:|error [A-Z]+\d+|fatal error|FAILED:|\*\*\*Failed|\*\*\*Exception|"
    r"Timeout|LNK\d+|undefined reference|internal compiler error|error\(s\))"
)
CONTEXT_AFTER = 8
MAX_ANNOTATIONS = 10
MAX_MESSAGE = 3500


def escape(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


blocks = []
seen = set()
for name in sys.argv[1:]:
    path = Path(name)
    if not path.is_file():
        continue
    lines = path.read_text(errors="replace").splitlines()
    i = 0
    while i < len(lines):
        if PATTERNS.search(lines[i]):
            block = lines[i:i + 1 + CONTEXT_AFTER]
            key = lines[i].strip()
            if key not in seen:
                seen.add(key)
                blocks.append(f"[{path.name}:{i + 1}]\n" + "\n".join(block))
            i += 1 + CONTEXT_AFTER
        else:
            i += 1

if not blocks:
    print("::error::no recognizable error lines in " + " ".join(sys.argv[1:]))
    sys.exit(0)

# Pack the blocks into at most MAX_ANNOTATIONS messages so nothing early gets dropped.
messages = []
current = ""
for block in blocks:
    if current and len(current) + len(block) > MAX_MESSAGE:
        messages.append(current)
        current = ""
        if len(messages) == MAX_ANNOTATIONS:
            break
    current += block[:MAX_MESSAGE] + "\n\n"
if current and len(messages) < MAX_ANNOTATIONS:
    messages.append(current)
for index, message in enumerate(messages):
    print(f"::error title=log errors {index + 1}/{len(messages)}::{escape(message)}")
print(f"{len(blocks)} error blocks, {len(messages)} annotations")
