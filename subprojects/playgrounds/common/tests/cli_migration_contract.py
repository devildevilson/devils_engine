"""Every migrated schema refuses before resources and preserves typed numeric boundaries."""
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, args, expected):
    result = subprocess.run([str(binary), *args], text=True, encoding="utf-8", capture_output=True, timeout=60)
    assert result.returncode == expected, (binary.name, args, result.returncode, result.stdout[-800:], result.stderr[-800:])
    return result


def main():
    binaries = [Path(s).resolve() for s in sys.argv[1:]]
    assert len(binaries) == 7
    for binary in binaries:
        for flag in ('--help', '-h'):
            result = run(binary, [flag], 0)
            assert '--seed' in result.stdout and not result.stderr
        for args in (['--unknown'], ['--seed'], ['--seed=-1'], ['--seed=12junk'],
                     ['--seed=18446744073709551616'], ['--seed=1', '--seed=2']):
            result = run(binary, args, 2)
            assert result.stderr and not result.stdout, (binary.name, args)
    for index in (0, 1):
        binary = binaries[index]
        for text in ('x', '=1', 'x=nan', 'x=1junk', 'x=1=2'):
            run(binary, ['--set', text], 2)
    malformed = {1: ('--probe', '1:2:3:4'), 3: ('--window','1,2,3,4,5'),
                 5: ('--sector','1,2,3'), 6: ('--position','1,2,3junk')}
    for index, (flag, value) in malformed.items():
        run(binaries[index], [flag, value], 2)
    for index, flag in ((0,'--distance'),(1,'--flight'),(5,'--flight'),(6,'--speed')):
        run(binaries[index], [flag, 'nan'], 2)
    seed = '18446744073709551615'
    with tempfile.TemporaryDirectory(prefix='devils_cli_migration_') as folder:
        folder = Path(folder)
        for index, common, flag, suffix in [
                (3, ['--mode','building'], '--dump', '.ppm'),
                (4, ['--no-map'], '--svg', '.svg'),
                (5, ['--sector=-2,1'], '--map', '.svg')]:
            a, b = folder/f'{index}a{suffix}', folder/f'{index}b{suffix}'
            run(binaries[index], [*common, f'--seed={seed}', f'{flag}={a}'], 0)
            run(binaries[index], [*common, '--seed', seed, flag, str(b)], 0)
            assert a.read_bytes() == b.read_bytes(), binaries[index].name
        # Viewer arguments reach deterministic pre-window paths, without requiring GPU/display.
        result = run(binaries[6], ['--seed',seed,'--position=-1.5,2,-3','--module=missing_cli_test_module'], 1)
        assert 'missing_cli_test_module' in result.stdout + result.stderr
    # Help shows real defaults, rather than the old handwritten GN02 values.
    assert '262144' in run(binaries[0], ['--help'], 0).stdout
    assert '128' in run(binaries[4], ['--help'], 0).stdout


if __name__ == '__main__':
    main()
