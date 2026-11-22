import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    source = (args.source or tests.parent / 'drm/freedreno_ringbuffer_sp.c').read_text()
    functions = source[source.index('uint32_t\nfd_submit_sp_bo_access('):
                       source.index('\nstatic void\nfd_submit_suballoc_ring_bo(')]
    with tempfile.TemporaryDirectory(prefix='freedreno-bo-access-test-') as directory:
        directory = Path(directory)
        (directory / 'bo_access_under_test.h').write_text(functions + '\n')
        binary = directory / 'bo_access_test'
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O0',
                        '-I', str(directory), '-o', str(binary),
                        str(tests / 'bo_access_test.c')], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    main()
