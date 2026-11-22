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
    source = (args.source or tests.parent / 'drm/freedreno_pipe.c').read_text()
    function = source[source.index('struct fd_pipe *\nfd_pipe_new2('):
                      source.index('\nstruct fd_pipe *\nfd_pipe_new(')]
    with tempfile.TemporaryDirectory(prefix='freedreno-pipe-test-') as directory:
        directory = Path(directory)
        (directory / 'pipe_under_test.h').write_text(function + '\n')
        binary = directory / 'pipe_creation_test'
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O0',
                        '-I', str(directory), '-o', str(binary),
                        str(tests / 'pipe_creation_test.c')], check=True)
        for scenario in range(6):
            subprocess.run([str(binary), str(scenario)], check=True, timeout=5)


if __name__ == '__main__':
    main()
