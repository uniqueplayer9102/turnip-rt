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
    source = (args.source or tests.parent / 'radish/rd_image.c').read_text()
    function = source[source.index('static bool\nrd_image_tiled_fp16_supported('):
                      source.index('\nstatic bool\nrd_image_layout(')]
    with tempfile.TemporaryDirectory(prefix='radish-fp16-layout-test-') as directory:
        directory = Path(directory)
        (directory / 'fp16_layout_under_test.h').write_text(function + '\n')
        binary = directory / 'test'
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O2',
                        '-I', str(directory), '-o', str(binary),
                        str(tests / 'radish_fp16_layout_test.c')], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    main()
