#!/usr/bin/env python3
"""Run the independent viewer acceptance flight and record Linux process memory."""
import argparse
import json
from pathlib import Path
import resource
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--frames', type=int, default=3600)
parser.add_argument('--seed', type=int, default=17)
parser.add_argument('--workers', type=int, default=6)
parser.add_argument('--turn-seconds', type=float, default=10)
parser.add_argument('--view', default='')
parser.add_argument('--log', type=Path, default=Path('/tmp/gn09_next_acceptance.log'))
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
binary = root / 'build-release/subprojects/playgrounds/GN09_local_adventure/bin/GN09_next_viewer'
command = [str(binary), f'--frames={args.frames}', f'--seed={args.seed}',
           f'--workers={args.workers}', f'--turn-seconds={args.turn_seconds}',
           '--flight=48', '--guard=-1']
if args.view:
    command.append(f'--view={args.view}')
samples = []
began = time.monotonic()
with args.log.open('w') as log:
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=root)
    try:
        while process.poll() is None:
            try:
                status = Path(f'/proc/{process.pid}/status').read_text()
                rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:'))
                samples.append({'seconds': round(time.monotonic()-began, 3), 'rss_mib': round(rss/1024, 1)})
            except (OSError, StopIteration):
                pass
            time.sleep(1)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait()
report = {'command': command, 'exit_code': process.returncode,
          'elapsed_seconds': round(time.monotonic()-began, 3),
          'peak_rss_mib': round(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss/1024, 1),
          'memory_samples': samples}
report_path = args.log.with_suffix('.json')
report_path.write_text(json.dumps(report, indent=2)+'\n')
print(f'Viewer log: {args.log}; memory report: {report_path}')
raise SystemExit(process.returncode)
