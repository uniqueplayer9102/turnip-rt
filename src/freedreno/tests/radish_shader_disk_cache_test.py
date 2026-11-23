import argparse
import os
from pathlib import Path
import re
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
    includes = ['-I', str(args.include)] if args.include else []
    with tempfile.TemporaryDirectory(prefix='radish-shader-cache-test-') as directory:
        directory = Path(directory)
        programs = {}
        for name, stages in (('radish_multi_draw_probe', ('vert', 'frag')),
                             ('radish_compute_cache_probe', ('comp',))):
            binary = directory / name
            subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O2',
                            *includes, '-o', str(binary), str(tests / f'{name}.c'),
                            *shlex.split(args.vulkan_library)], check=True)
            command = [str(binary)]
            for stage in stages:
                shader = directory / f'{name}.{stage}.spv'
                subprocess.run([*shlex.split(args.glslc), str(tests / f'{name}.{stage}'),
                                '-o', str(shader)], check=True)
                command.append(str(shader))
            programs[name] = command
        env = dict(os.environ, MESA_SHADER_CACHE_DIR=str(directory / 'cache'),
                   MESA_SHADER_CACHE_SHOW_STATS='true', RADISH_GMEM='0',
                   RADISH_TILED_FP16='0', RADISH_GMEM_ELIDE_CLEAR_LOAD='0',
                   RADISH_RENDER_GPU_TIMING='0')
        env.pop('MESA_SHADER_CACHE_DISABLE', None)
        for name, wavesizes in (('radish_multi_draw_probe', ('single', 'double', 'any')),
                                ('radish_compute_cache_probe', ('single', 'double', 'any'))):
            for wavesize in wavesizes:
                for phase, enabled in (('disabled', '0'), ('cold', '1'), ('warm', '1')):
                    result = subprocess.run(programs[name], capture_output=True, text=True,
                        env=dict(env, RADISH_SHADER_DISK_CACHE=enabled, RADISH_WAVESIZE=wavesize,
                                 RADISH_FS_WAVESIZE=wavesize),
                        timeout=45)
                    text = result.stdout + result.stderr
                    if result.returncode:
                        raise RuntimeError(text[-6000:])
                    if name == 'radish_multi_draw_probe':
                        passes = re.findall(r'PIXELS mode=\d+ checked=16384 PASS', text)
                        expected = 7
                        timing = re.search(r'PIPELINE_CREATE ms=([0-9.]+)', text)
                    else:
                        passes = re.findall(r'COMPUTE_READBACK[^\n]+PASS', text)
                        expected = 3
                        timing = re.search(r'COMPUTE_PIPELINE_CREATE ms=([0-9.]+)', text)
                    if len(passes) != expected or not timing:
                        raise RuntimeError(text[-6000:])
                    stats = re.findall(r'disk shader cache:\s+hits = (\d+), misses = (\d+)', text)
                    if phase == 'warm' and (not stats or int(stats[0][0]) == 0 or int(stats[0][1])):
                        raise RuntimeError('Expected warm disk-cache hits:\n' + text[-6000:])
                    print(f'{name} wavesize={wavesize} {phase}: '
                          f'pipeline={timing[1]}ms stats={stats} PASS', flush=True)
        print('SHADER_DISK_CACHE PASS: graphics, indexed draws, secondary commands, '
              'shared-memory compute and wavesize-specific cold/warm entries', flush=True)


if __name__ == '__main__':
    main()
