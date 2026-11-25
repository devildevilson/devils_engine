"""Command-line session modes of frontier_online_server as real processes.

1. --break-compat: two incompatible installations are refused WITH their reason, a compatible one
   joins, and the authority records exactly that.
2. --play: a session for people. A participant that leaves early (its own shorter run closes the
   connection) gets a canonical leave, another one runs to the end with the authority's root, and
   the authority succeeds — departures are normal there, not a failed run.
"""

import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_live_process_smoke import Process, exit_code, readable, value  # noqa: E402


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def report(label: str, processes: list[tuple[str, Process]]) -> None:
    print(f"session cli smoke failed: {label}", file=sys.stderr)
    for name, process in processes:
        output = process.stop()
        print(f"--- {name} (exit {exit_code(process.popen.returncode)}) ---\n{readable(output)}",
              file=sys.stderr)


def break_compat(binary: str) -> bool:
    port = free_port()
    authority = Process([binary, f"--listen={port}", "--expect-joins=1", "--timeout=20"])
    processes = [("authority", authority)]
    time.sleep(0.4)
    for kind in ("protocol", "content"):
        refused = Process([binary, f"--join=127.0.0.1:{port}", f"--break-compat={kind}"])
        processes.append((f"incompatible {kind}", refused))
        output = refused.finish(timeout=20)
        if refused.popen.returncode != 0 or "refused as declared" not in output:
            report(f"--break-compat={kind} was not refused with its reason", processes)
            return False
    honest = Process([binary, f"--join=127.0.0.1:{port}"])
    processes.append(("compatible", honest))
    if honest.finish(timeout=20) and honest.popen.returncode != 0:
        report("the compatible installation did not join", processes)
        return False
    output = authority.finish(timeout=25)
    if authority.popen.returncode != 0 or "listen.joins = 1 of 1" not in output or \
       value(output, "listen.refused") != 2:
        report("the authority did not record one join and two refusals", processes)
        return False
    return True


def play_session(binary: str) -> bool:
    port = free_port()
    common = ["--live", "--ticks=600", "--timeout=30", "--verify"]
    authority = Process([binary, *common, "--play", f"--listen={port}", "--live-loopback",
                         "--expect-joins=2", "--actors=64"])
    processes = [("authority", authority)]
    time.sleep(0.4)
    # Уходит сам на тике 200: закрытое соединение авторитет замечает сразу (убитый процесс транспорт
    # замечал бы только по своему сроку, около 10 с).
    leaving = Process([binary, "--live", "--ticks=200", "--timeout=30", "--verify",
                       f"--join=127.0.0.1:{port}"], watch="live.follower.joined_tick = ")
    processes.append(("leaving follower", leaving))
    leaving.seen.wait(timeout=20)
    if not leaving.watched():
        report("the first follower did not join", processes)
        return False
    staying = Process([binary, *common, f"--join=127.0.0.1:{port}"])
    processes.append(("staying follower", staying))
    leaving.finish(timeout=30)
    staying_output = staying.finish(timeout=40)
    output = authority.finish(timeout=40)
    if authority.popen.returncode != 0 or staying.popen.returncode != 0 or \
       leaving.popen.returncode != 0:
        report("the play session failed", processes)
        return False
    if value(output, "live.authority.tick") != 600 or value(output, "live.authority.players") != 2 or \
       value(output, "live.authority.departed") < 1 or \
       value(staying_output, "live.follower.root") != value(output, "live.authority.root"):
        report("departure, players or the final root differ", processes)
        return False
    return True


def main() -> int:
    if len(sys.argv) != 2:
        raise RuntimeError("expected the server binary")
    binary = sys.argv[1]
    if not break_compat(binary) or not play_session(binary):
        return 1
    print("session cli: two refusals with reasons and one join; play session with a departure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
