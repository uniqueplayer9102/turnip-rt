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
    source = (args.source or tests.parent / 'radish/rd_graphics.c').read_text()
    function = source[source.index('static bool\nrd_gmem_tile_has_draws('):
                      source.index('\nstatic void\nrd_gmem_emit_msaa_once(')]
    with tempfile.TemporaryDirectory(prefix='radish-gmem-damage-test-') as directory:
        directory = Path(directory)
        (directory / 'gmem_damage_under_test.h').write_text(function + '\n')
        binary = directory / 'test'
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O2',
                        '-I', str(directory), '-o', str(binary),
                        str(tests / 'radish_gmem_damage_test.c')], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    main()
