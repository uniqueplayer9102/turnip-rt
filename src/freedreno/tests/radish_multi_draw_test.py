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
    functions = source[source.index('static struct rd_draw *\nrd_record_multi_draw('):
                       source.index('\nstatic void\nrd_record_indirect(')]
    with tempfile.TemporaryDirectory(prefix='radish-multi-draw-test-') as directory:
        directory = Path(directory)
        (directory / 'multi_draw_under_test.h').write_text(functions + '\n')
        binary = directory / 'radish_multi_draw_test'
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O2',
                        '-I', str(directory), '-o', str(binary),
                        str(tests / 'radish_multi_draw_test.c')], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    main()
