import argparse
from collections import deque
import os
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import tempfile


def test_gmem_subpasses(command, width, height, environment):
    for mode in ('retain', 'discard', 'nonlocal'):
        hashes = []
        for enabled in (0, 1):
            env = dict(environment, RADISH_GMEM_FP16_SUBPASS=str(enabled),
                       RADISH_PROBE_GMEM_SUBPASSES=mode, RADISH_PROBE_FRAMES='3')
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
            text = result.stdout + result.stderr
            if result.returncode:
                raise RuntimeError(text[-6000:])
            frames = re.findall(rf'CONVERSION_READBACK frame=\d+ pixels={width*height} hash=([0-9a-f]+) PASS', text)
            if len(frames) != 3:
                raise RuntimeError(text[-6000:])
            hashes.append(frames)
            tiles = re.findall(r'RADISH_GMEM_FP16_SUBPASS tiles=(\d+) bin=\d+x\d+ source_stores=(\d+)', text)
            if enabled and mode != 'nonlocal':
                if len(tiles) != 3 or any(int(count) <= 1 for count, _ in tiles):
                    raise RuntimeError('Expected three multi-tile GMEM passes:\n' + text[-6000:])
                stored = [int(stores) > 0 for _, stores in tiles]
                if stored != ([True, True, False] if mode == 'retain' else [False] * 3):
                    raise RuntimeError('Unexpected FP16 tile stores:\n' + text[-6000:])
            elif tiles:
                raise RuntimeError('Unexpected GMEM fusion:\n' + text[-6000:])
        if hashes[0] != hashes[1]:
            raise RuntimeError(f'{width}x{height} {mode}: GMEM and sysmem pixels differ')
        print(f'{width}x{height} GMEM subpasses ({mode}): pixel readback PASS', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--glslc', default='glslc')
    parser.add_argument('--include', type=Path)
    parser.add_argument('--vulkan-library', default='-lvulkan')
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--rgba8', action='store_true')
    parser.add_argument('--fs-wavesize', choices=('single', 'double', 'any'), default='single')
    parser.add_argument('--disk-cache', action='store_true')
    parser.add_argument('--gmem-subpasses', action='store_true', help='test FP16-to-BGRA8 tile-local subpasses on Radish')
    args = parser.parse_args()
    if args.gmem_subpasses and (args.benchmark or args.rgba8 or args.disk_cache):
        parser.error('--gmem-subpasses is a standalone FP16 correctness run')
    tests = Path(__file__).resolve().parent
    includes = ['-I', str(args.include)] if args.include else []
    with tempfile.TemporaryDirectory(prefix='radish-fp16-render-test-') as directory:
        directory = Path(directory)
        shaders = []
        for name in ('radish_fp16_render_probe.vert', 'radish_fp16_render_copy.frag',
                     'radish_fp16_render_pattern.frag'):
            shader = directory / f'{name}.spv'
            subprocess.run([*shlex.split(args.glslc), str(tests / name),
                            '-o', str(shader)], check=True)
            shaders.append(str(shader))
        dimensions = ((1024, 2048),) if args.benchmark else ((1024, 2048), (1003, 1997))
        for width, height in dimensions:
            binary = directory / f'probe-{width}-{height}'
            subprocess.run([*shlex.split(args.cc), '-Wall', '-Wextra', '-Werror', '-O2',
                            f'-DW={width}u', f'-DH={height}u',
                            *(['-DSOURCE_FORMAT=VK_FORMAT_R8G8B8A8_UNORM'] if args.rgba8 else []),
                            *includes, '-o', str(binary),
                            str(tests / 'radish_fp16_render_probe.c'),
                            *shlex.split(args.vulkan_library)], check=True)
            if args.gmem_subpasses:
                env = dict(os.environ, RADISH_GMEM='1', RADISH_GMEM_MIN_DRAWS='100',
                           RADISH_GMEM_DISABLE_FILE=str(directory / 'enable-gmem'),
                           RADISH_GMEM_DUMP='1', RADISH_GMEM_CONVERSION_PASS='0',
                           RADISH_TILED_FP16='0', RADISH_RENDER_BATCH='0', RADISH_ASYNC_QUEUE_SYNC='0',
                           RADISH_PROFILE='0', RADISH_RENDER_GPU_TIMING='0', RADISH_RENDER_GPU_DETAIL='0',
                           RADISH_GMEM_STOP='-1', RADISH_PROBE_MIXED='1', RADISH_PROBE_EMPTY='1')
                env.pop('RADISH_TRACE', None)
                test_gmem_subpasses([str(binary), *shaders[:2], 'gpu', shaders[2]], width, height, env)
                continue
            combinations = ((1, 0, 0, 1), (1, 0, 1, 1), (1, 1, 1, 1), (1, 1, 0, 1)) if args.benchmark else (
                (0, 0, 0, 0), (0, 1, 0, 0), (1, 0, 0, 0), (1, 0, 1, 0),
                (1, 0, 1, 1), (1, 1, 0, 0), (1, 1, 1, 0), (1, 1, 1, 1))
            hashes = []
            frames = 20 if args.benchmark else 3
            for arm, (gmem, tiled, skip, cull) in enumerate(combinations):
                env = dict(os.environ, RADISH_GMEM=str(gmem), RADISH_GMEM_MIN_DRAWS='1',
                    RADISH_GMEM_FP16_SUBPASS='0',
                    RADISH_TILED_FP16=str(tiled), RADISH_TILED_FP16_DEBUG='1',
                    RADISH_GMEM_DAMAGE_TILES=str(skip), RADISH_GMEM_CULL=str(cull),
                    RADISH_GMEM_DUMP='1', RADISH_GMEM_ELIDE_CLEAR_LOAD='0',
                    RADISH_RENDER_GPU_TIMING='1' if args.benchmark else '0',
                    RADISH_RENDER_GPU_DETAIL='0', RADISH_GMEM_CONVERSION_PASS='0',
                    RADISH_GMEM_FP16_BIN_W='0', RADISH_GMEM_FP16_BIN_H='0',
                    RADISH_FS_WAVESIZE=args.fs_wavesize, RADISH_PROBE_FRAMES=str(frames),
                    RADISH_SHADER_DISK_CACHE='1' if args.disk_cache else '0')
                env.pop('RADISH_PROBE_GMEM_SUBPASSES', None)
                if args.disk_cache:
                    env.pop('MESA_SHADER_CACHE_DISABLE', None)
                    env.update(MESA_SHADER_CACHE_DIR=str(directory / 'cache'),
                               MESA_SHADER_CACHE_SHOW_STATS='true')
                if args.benchmark:
                    env.pop('RADISH_PROBE_MIXED', None)
                    env.pop('RADISH_PROBE_EMPTY', None)
                else:
                    env['RADISH_PROBE_MIXED'] = '1'
                    env['RADISH_PROBE_EMPTY'] = '1'
                command = [str(binary), *shaders[:2], 'gpu', shaders[2]]
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=100)
                text = result.stdout + result.stderr
                if result.returncode:
                    raise RuntimeError(text[-6000:])
                current = re.findall(rf'CONVERSION_READBACK frame=\d+ pixels={width*height} '
                                     r'hash=([0-9a-f]+) PASS', text)
                if len(current) != frames:
                    raise RuntimeError(text[-6000:])
                layouts = re.findall(r'RADISH_FP16_LAYOUT tile=(\d+)', text)
                expected_layouts = [] if args.rgba8 else (['3'] if tiled else ['0'])
                if layouts != expected_layouts:
                    raise RuntimeError(text[-6000:])
                skipped = [int(value) for value in re.findall(r'radish gmem damage: skipped_tiles=(\d+)', text)]
                if skip and (not skipped or not max(skipped)):
                    raise RuntimeError('No untouched tiles skipped:\n' + text[-6000:])
                hashes.append(current)
                if args.disk_cache and arm:
                    stats = re.findall(r'disk shader cache:\s+hits = (\d+), misses = (\d+)', text)
                    if not stats or int(stats[0][0]) == 0 or int(stats[0][1]):
                        raise RuntimeError('Expected warm shader-cache hits:\n' + text[-6000:])
                suffix = ''
                if args.benchmark:
                    pending = deque()
                    values = []
                    for line in text.splitlines():
                        shape = re.search(r'RADISH_RENDER_SELECTION draws=(\d+) format=(\d+)', line)
                        if shape and int(shape[1]):
                            pending.append(int(shape[2]))
                        timing = re.search(r'RADISH_GPU_RENDER_TIMING valid=(\d+).*gpu_ns=(\d+)', line)
                        if timing:
                            if not pending or timing[1] != '1':
                                raise RuntimeError(line)
                            if pending.popleft() == (37 if args.rgba8 else 97):
                                values.append(int(timing[2]) / 1e6)
                    if len(values) != frames:
                        raise RuntimeError('Missing GPU timings')
                    suffix = f'; partial-pass median={statistics.median(values[5:]):.3f}ms'
                print(f'{width}x{height} GMEM={gmem} TILED={tiled} SKIP={skip} CULL={cull}: '
                      f'{frames*width*height:,} pixels PASS{suffix}', flush=True)
            if any(value != hashes[0] for value in hashes):
                raise RuntimeError('Variant pixel hashes differ')
            env.pop('RADISH_PROBE_MIXED', None)
            env.pop('RADISH_PROBE_EMPTY', None)
            env.update(RADISH_TILED_FP16='1', RADISH_PROBE_FRAMES='2', RADISH_RENDER_GPU_TIMING='0')
            result = subprocess.run([str(binary), *shaders[:2], 'clear'], env=env,
                                    capture_output=True, text=True, timeout=50)
            text = result.stdout + result.stderr
            if result.returncode or (not args.rgba8 and 'RADISH_FP16_LAYOUT tile=0' not in text) or text.count(' PASS') != 2:
                raise RuntimeError('Transfer fallback failed:\n' + text[-6000:])
            print('Transfer fallback PASS' if args.rgba8 else
                  'Transfer-capable FP16 image remains linear PASS', flush=True)
        if not args.gmem_subpasses:
            print('FP16_RENDER PASS: tiling, partial updates, mixed pipelines, clipping and retained pixels', flush=True)


if __name__ == '__main__':
    main()
