"""Three real processes: a moving authority and two headless followers."""

import re
import socket
import subprocess
import sys
import time


def value(output: str, name: str) -> int:
    match = re.search(rf"^{re.escape(name)} = (\d+)$", output, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing {name}")
    return int(match.group(1))


def main() -> int:
    if len(sys.argv) not in (2, 3, 4):
        raise RuntimeError("expected binary, optional actor count and optional resource root")
    binary = sys.argv[1]
    actors = int(sys.argv[2]) if len(sys.argv) >= 3 else 128
    resources = [f"--resources={sys.argv[3]}"] if len(sys.argv) == 4 else []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    common = ["--live", "--ticks=180", "--timeout=20", "--verify", *resources]
    processes: list[subprocess.Popen[str]] = []
    outputs: list[str] = []
    try:
        authority = subprocess.Popen(
            [binary, *common, f"--listen={port}", "--live-loopback", "--expect-joins=2",
             f"--actors={actors}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        processes.append(authority)
        time.sleep(0.4)
        follower_a = subprocess.Popen(
            [binary, *common, f"--join=127.0.0.1:{port}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        processes.append(follower_a)
        first_prefix = ""
        assert follower_a.stdout is not None
        for line in follower_a.stdout:
            first_prefix += line
            if line.startswith("live.follower.joined_tick = "):
                break
        else:
            raise RuntimeError("first follower exited before its join was confirmed")
        time.sleep(0.4)
        follower_b = subprocess.Popen(
            [binary, *common, f"--join=127.0.0.1:{port}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        processes.append(follower_b)

        for index, process in enumerate(processes):
            output, _ = process.communicate(timeout=25)
            if index == 1:
                output = first_prefix + output
            outputs.append(output)
        if any(process.returncode != 0 for process in processes):
            raise RuntimeError("one or more live processes failed")

        server_root = value(outputs[0], "live.authority.root")
        if value(outputs[0], "live.authority.tick") != 180 or \
           value(outputs[0], "live.authority.joins") != 2 or \
           value(outputs[0], "live.authority.inputs") != 2 or \
           value(outputs[0], "live.authority.dropped_inputs") != 0 or \
           value(outputs[0], "live.authority.checkpoint_bytes_sent") == 0 or \
           value(outputs[0], "live.authority.canonical_bytes_sent") == 0:
            raise RuntimeError("authority did not complete the declared session")
        first_checkpoint = value(outputs[1], "live.follower.checkpoint_tick")
        second_checkpoint = value(outputs[2], "live.follower.checkpoint_tick")
        if not first_checkpoint < second_checkpoint:
            raise RuntimeError("second follower did not join a moving world")
        for output in outputs[1:]:
            if value(output, "live.follower.tick") != 180 or \
               value(output, "live.follower.root") != server_root or \
               value(output, "live.follower.corrected") != 1 or \
               value(output, "live.follower.own_inputs") != 1 or \
               value(output, "live.follower.bundles_verified") != \
                   180 - value(output, "live.follower.checkpoint_tick") or \
               value(output, "live.follower.checkpoint_bytes") == 0 or \
               value(output, "live.follower.canonical_bytes") == 0:
                raise RuntimeError("follower root, replay or input differs")
        print(f"live processes: {actors} actors, checkpoints {first_checkpoint}/{second_checkpoint}, "
              f"tick 180 root {server_root}, two inputs, two repairs")
        return 0
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"live process smoke failed: {error}", file=sys.stderr)
        for process in processes:
            if process.poll() is None:
                process.kill()
        for index, process in enumerate(processes):
            if index >= len(outputs):
                output, _ = process.communicate()
                if index == 1 and "first_prefix" in locals():
                    output = first_prefix + output
                outputs.append(output)
        for label, output in zip(("authority", "follower A", "follower B"), outputs):
            print(f"--- {label} ---\n{output}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
