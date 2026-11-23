import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--glslc', default='glslc')
    parser.add_argument('--include', type=Path)
    parser.add_argument('--vulkan-library', default='-lvulkan')
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix='radish-multi-draw-probe-') as directory:
        directory = Path(directory)
        binary = directory / 'probe'
        includes = ['-I', str(args.include)] if args.include else []
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O2',
                        *includes, '-o', str(binary),
                        str(tests / 'radish_multi_draw_probe.c'),
                        *shlex.split(args.vulkan_library)], check=True)
        shaders = []
        for stage in ('vert', 'frag'):
            shader = directory / f'{stage}.spv'
            subprocess.run([*shlex.split(args.glslc),
                            str(tests / f'radish_multi_draw_probe.{stage}'),
                            '-o', str(shader)], check=True)
            shaders.append(str(shader))
        subprocess.run([str(binary), *shaders], check=True, timeout=40)


if __name__ == '__main__':
    main()
