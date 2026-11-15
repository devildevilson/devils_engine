"""Turn error lines of CI logs into workflow annotations.

Job logs need authentication to download, annotations do not: this makes the reason of a failed
step readable from the public API. Usage: report_errors.py LOG [LOG...]
"""
import re
import sys
from pathlib import Path

# "error:" must not be followed by another ':' (utils::error::error in MSVC warning text).
PATTERNS = re.compile(
    r"(CMake Error|\berror(?: [A-Z]+\d+)?:(?!:)|fatal error|FAILED:|\*\*\*Failed|\*\*\*Exception|"
    r"Timeout|error LNK\d+|undefined reference|internal compiler error|[1-9]\d* Error\(s\))"
)
# MSVC /Wall prints thousands of warnings whose text mentions functions named error.
IGNORED = re.compile(r"warning [A-Z]+\d+:")
NOISE = re.compile(r"\w+\.lib\([^)]*\)|\[[^\]]*\.vcxproj\]")
CONTEXT_AFTER = 8
TEST_CONTEXT_AFTER = 40
MAX_ANNOTATIONS = 10
MAX_MESSAGE = 3500


def escape(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


PROJECT = re.compile(r"\[[^\]]*[\\/]([^\\/\]]+)\.vcxproj\]")
PER_PROJECT = 2

blocks = []
seen = set()
for name in sys.argv[1:]:
    path = Path(name)
    if not path.is_file():
        continue
    lines = path.read_text(errors="replace").splitlines()
    if path.name == "msbuild-errors.log":
        # Only errors here: the first few distinct ones of every failing project give the breadth.
        per_project = {}
        for line in lines:
            match = PROJECT.search(line)
            project = match.group(1) if match else "?"
            text = re.sub(r"^\s*\d+>", "", NOISE.sub("", line)).strip()
            found = per_project.setdefault(project, [])
            if text and text not in found and len(found) < PER_PROJECT:
                found.append(text)
        for project, found in per_project.items():
            seen.update(found)
            blocks.append(f"[{project}]\n" + "\n".join(found))
        continue
    i = 0
    while i < len(lines):
        if PATTERNS.search(lines[i]) and not IGNORED.search(lines[i]):
            # A failed test's own output follows its ctest line: keep more of it.
            after = TEST_CONTEXT_AFTER if "***" in lines[i] else CONTEXT_AFTER
            block = lines[i:i + 1 + after]
            # One error repeated per object file or per project is reported once.
            key = re.sub(r"^\s*\d+>", "", NOISE.sub("", lines[i])).strip()
            if key not in seen:
                seen.add(key)
                blocks.append(f"[{path.name}:{i + 1}]\n" + "\n".join(block))
            i += 1 + after
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
