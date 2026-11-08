#!/usr/bin/env python3
"""Check the executable boundary: help/refusal precede generation; argv spellings agree."""

from pathlib import Path
import subprocess
import sys
import tempfile


def run(executable, args, expected):
  result = subprocess.run([str(executable), *args], text=True, encoding="utf-8", capture_output=True, timeout=30)
  if result.returncode != expected:
    raise AssertionError(f"{executable.name} {args}: exit {result.returncode}, expected {expected}\n"
                         f"{result.stdout}\n{result.stderr}")
  return result


def main():
  gn01, gn05 = (Path(arg).resolve() for arg in sys.argv[1:])
  with tempfile.TemporaryDirectory(prefix="devils_cli_") as temporary:
    # No resource lookup or output publication is allowed on these parse-only paths.
    for executable in (gn01, gn05):
      help_result = run(executable, ["--help"], 0)
      assert "--size" in help_result.stdout and "--seed" in help_result.stdout
      assert ("1024" if executable == gn01 else "96") in help_result.stdout
      assert not help_result.stderr
      for args in (["--unknown"], ["--size"], ["--size=-1"], ["--size=12junk"],
                   ["--seed=-1"], ["--seed=18446744073709551616"], ["--seed=1", "--seed=2"]):
        result = run(executable, args, 2)
        assert result.stderr and not result.stdout
      output = Path(temporary) / "must_not_exist.ppm"
      run(gn05, [f"--dump={output}", "--mode=unknown"], 2)
      assert not output.exists()

    run(gn01, ["--layout=invalid"], 2)
    run(gn01, ["--size=0"], 2)
    run(gn05, ["--size=1"], 2)
    run(gn05, ["--rollbacks=-2"], 2)

    # Full-width seeds must survive conversion, and both common option spellings must agree.
    seed = "18446744073709551615"
    for layout in ("aos", "soa"):
      result = run(gn01, ["--size", "16", "--sites", "8", "--threads", "1",
                         "--seed", seed, "--layout", layout], 0)
      assert f"раскладка {layout}" in result.stdout
    for mode in ("declared", "learned", "graph"):
      first = Path(temporary) / f"{mode}_equals.ppm"
      second = Path(temporary) / f"{mode}_separate.ppm"
      result = run(gn05, ["--size=16", f"--seed={seed}", f"--mode={mode}", f"--dump={first}"], 0)
      assert seed in result.stdout
      other = run(gn05, ["--size", "16", "--seed", seed, "--mode", mode, "--dump", str(second)], 0)
      if mode == "graph":
        # The existing sphere mode reports data and deliberately does not export a raster PPM.
        assert not first.exists() and not second.exists()
        stable = lambda text: [line for line in text.splitlines() if "решено за" not in line]
        assert stable(result.stdout) == stable(other.stdout)
      else:
        assert first.read_bytes() == second.read_bytes(), mode


if __name__ == "__main__":
  main()
