"""Three real processes: a moving authority and two headless followers."""

import re
import socket
import subprocess
import sys
import threading
import time


def value(output: str, name: str) -> int:
    match = re.search(rf"^{re.escape(name)} = (\d+)$", output, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing {name}")
    return int(match.group(1))


RESOURCE_SKIP = "Could not find proper type for resource"


def readable(output: str) -> str:
    # Каждый процесс начинает с ~30 предупреждений demiurg о ресурсах без типа (headless-сервер не
    # регистрирует окно, звук и шрифты). В отчёте CI они вытесняли из окна саму причину.
    lines = output.splitlines()
    skipped = sum(RESOURCE_SKIP in line for line in lines)
    kept = [line for line in lines if RESOURCE_SKIP not in line]
    if skipped:
        kept.insert(0, f"({skipped} resource-type warnings omitted)")
    return "\n".join(kept)


def exit_code(code: int | None) -> str:
    # Аварийное завершение на Windows приходит кодом NTSTATUS: 0xC0000005 — нарушение доступа,
    # 0xC00000FD — переполнение стека, 0xC0000409 — abort/fast fail. Процесс, убитый тестом, — свой код.
    if code is None:
        return "running"
    return f"{code} (0x{code & 0xFFFFFFFF:08X})" if code < 0 or code > 255 else str(code)


class Process:
    """A child whose output is drained by its own thread from the very start.

    A pipe nobody reads fills up and blocks the writer: on Windows an anonymous pipe holds ~4 KB,
    and the authority's ~30 startup warnings filled it, so it never reached listening (on Linux the
    64 KB pipe hid this). Every child is read continuously, whatever the test waits for.
    """

    def __init__(self, command: list[str], watch: str | None = None) -> None:
        self.lines: list[str] = []
        self.seen = threading.Event()
        self.watch = watch
        self.popen = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, errors="replace")
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()

    def _drain(self) -> None:
        assert self.popen.stdout is not None
        for line in self.popen.stdout:
            self.lines.append(line)
            if self.watch is not None and line.startswith(self.watch):
                self.seen.set()
        self.seen.set()  # конец вывода тоже будит ждущего: он проверит, было ли событие

    def watched(self) -> bool:
        return self.watch is not None and any(line.startswith(self.watch) for line in self.lines)

    def finish(self, timeout: float) -> str:
        self.popen.wait(timeout=timeout)
        self.reader.join(timeout=5)
        return "".join(self.lines)

    def stop(self) -> str:
        if self.popen.poll() is None:
            self.popen.kill()
        self.popen.wait()
        self.reader.join(timeout=5)
        return "".join(self.lines)


def main() -> int:
    if len(sys.argv) not in (2, 3, 4):
        raise RuntimeError("expected binary, optional actor count and optional resource root")
    binary = sys.argv[1]
    actors = int(sys.argv[2]) if len(sys.argv) >= 3 else 128
    resources = [f"--resources={sys.argv[3]}"] if len(sys.argv) == 4 else []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    # --verbose: события рукопожатия и этапы запуска. Когда процесс снят снаружи, только они и
    # говорят, где он стоял.
    common = ["--live", "--ticks=180", "--timeout=20", "--verify", "--verbose", *resources]
    processes: list[Process] = []
    try:
        processes.append(Process(
            [binary, *common, f"--listen={port}", "--live-loopback", "--expect-joins=2",
             f"--actors={actors}"]))
        time.sleep(0.4)
        follower_a = Process([binary, *common, f"--join=127.0.0.1:{port}"],
                             watch="live.follower.joined_tick = ")
        processes.append(follower_a)
        follower_a.seen.wait(timeout=25)
        if not follower_a.watched():
            raise RuntimeError("first follower exited before its join was confirmed")
        time.sleep(0.4)
        processes.append(Process([binary, *common, f"--join=127.0.0.1:{port}"]))

        outputs = [process.finish(timeout=25) for process in processes]
        if any(process.popen.returncode != 0 for process in processes):
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
        for label, process in zip(("authority", "follower A", "follower B"), processes):
            output = process.stop()
            print(f"--- {label} (exit {exit_code(process.popen.returncode)}) ---\n{readable(output)}",
                  file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
