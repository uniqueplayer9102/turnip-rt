import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name, cpp=False):
    prefix = r'^static bool ' if cpp else r'^(?:static (?:inline )?)?(?:void|bool|unsigned|uint32_t|uint64_t|VkResult|nir_mem_access_size_align)\n'
    match = re.search(prefix + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.end()) + 3] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver-source', type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--include', type=Path)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    driver = args.driver_source or tests.parent / 'radish'
    graphics = (driver / 'rd_graphics.c').read_text()
    trace = (driver / 'rd_trace.c').read_text()
    resources = (driver / 'rd_resources.c').read_text()
    cache = (driver / 'rd_pipeline_cache.cpp').read_text()
    instance = (driver / 'rd_instance.c').read_text()
    queue = (driver / 'rd_queue.c').read_text()
    descriptor = (driver / 'rd_descriptor.c').read_text()
    event = (driver / 'rd_event.c').read_text()
    query = (driver / 'rd_query.c').read_text()
    sync_header = (driver.parent.parent / 'vulkan' / 'runtime' / 'vk_sync.h').read_text()
    direct = extract(graphics, 'rd_execute_render_direct')
    assert re.search(r'result\s*=\s*queries\s*\?\s*rd_finish_submit\(', direct)
    assert direct.index('result = queries ?') < direct.index('if (result == VK_SUCCESS && queries)')
    assert 'q.zero |= i != 0;' in graphics
    assert '.pipelineStatisticsQuery = false' in instance
    assert 'util_dynarray_append(' not in graphics
    pipeline = (driver / 'rd_pipeline.c').read_text()
    assert pipeline.index('rd_nir_lower_subword_storage(nir)') < pipeline.index(
       'rd_nir_lower_robust_buffers(nir)')
    assert graphics.index('rd_nir_lower_subword_storage(nir)') < graphics.index(
       'rd_nir_lower_robust_buffers(nir)')
    assert re.search(r'if \(draw->rasterizer_discard\)\s*\n\s*nr = 0;', graphics)
    assert re.search(r'rd_CmdWaitEvents2\([\s\S]{0,600}?if \(command->render\)', event)
    assert re.search(r'rd_CmdWriteTimestamp2\([\s\S]{0,600}?if \(command->render\)', query)
    assert 'rd_query_range(pool, query, 1)' in query[query.index('rd_CmdEndQuery('):][:400]
    assert 'rd_vertex_input_valid(info->pVertexInputState)' in graphics
    assert descriptor.count('rd_descriptor_span_valid(') >= 4
    assert 'RADISH_SUBMIT_DIAG' in queue
    assert 'compute_deferred' not in queue
    assert 'rd_compute_can_defer(async, deferred,' in queue
    assert 'device->publish_signal_fence' in queue
    with tempfile.TemporaryDirectory(prefix='radish-correctness-unit-') as directory:
        directory = Path(directory)
        (directory / 'trace_defs_under_test.h').write_text(
            (driver / 'rd_trace.h').read_text().replace('#include "util/u_dynarray.h"\n', ''))
        names = ('rd_render_append_query', 'rd_render_append_meta_draw', 'rd_primitive_count',
                 'rd_restart_counts', 'rd_vertex_fetch_size', 'rd_render_query_op',
                 'rd_render_needs_query_accounting', 'rd_account_draw')
        (directory / 'graphics_under_test.h').write_text(''.join(extract(graphics, name) for name in names))
        names = ('rd_trace_accumulate', 'rd_trace_submit_begin', 'rd_trace_submit_end',
                 'rd_trace_needs_resolve', 'rd_trace_command_begin', 'rd_trace_command_end',
                 'rd_trace_resolve')
        (directory / 'trace_under_test.h').write_text(''.join(extract(trace, name) for name in names))
        color = resources[resources.index('struct rd_border_color {'):resources.index('\n_Static_assert')]
        (directory / 'border_under_test.h').write_text(color + extract(resources, 'rd_init_border_colors'))
        (directory / 'texture_pitch_under_test.h').write_text(
            extract((driver / 'rd_image.h').read_text(), 'rd_texture_array_pitch_fits') +
            extract(resources, 'rd_resource_texture_pitch'))
        (directory / 'cache_under_test.h').write_text(extract(cache, 'resource_metadata_valid', True) +
                                                    extract(cache, 'variant_resources_valid', True))
        (directory / 'subword_under_test.h').write_text(
            extract((driver / 'rd_nir_robust.c').read_text(), 'rd_subword_storage_access'))
        (directory / 'blit_src_under_test.h').write_text(
            extract((driver / 'rd_image.c').read_text(), 'rd_image_required_blit_src'))
        (directory / 'device_extensions_under_test.h').write_text(
            extract(instance, 'rd_device_extensions_for_mode'))
        (directory / 'image_pitch_under_test.h').write_text(
            extract((driver / 'rd_image.c').read_text(), 'rd_image_sampled_pitch_supported'))
        (directory / 'sync_waits_under_test.h').write_text(
            extract(sync_header, 'vk_sync_waits_have_timeline'))
        (directory / 'sync_policy_under_test.h').write_text(''.join(
            extract(queue, name) for name in ('rd_compute_can_defer', 'rd_publish_signal_fences',
                                              'rd_render_exports_fence',
                                              'rd_render_query_before_work')))
        (directory / 'vertex_input_under_test.h').write_text(
            extract(graphics, 'rd_vertex_input_valid'))
        (directory / 'query_range_under_test.h').write_text(
            extract(query, 'rd_query_range'))
        (directory / 'descriptor_span_under_test.h').write_text(
            extract(descriptor, 'rd_descriptor_span_valid'))
        (directory / 'drain_index_under_test.h').write_text(
            extract(queue, 'rd_drain_index'))
        includes = ['-I', str(directory)]
        if args.include:
            includes += ['-I', str(args.include)]
        for compiler, source, standard in ((args.cc, 'radish_correctness_unit_test.c', '-std=gnu11'),
                                            (args.cxx, 'radish_cache_metadata_test.cpp', '-std=c++17')):
            binary = directory / Path(source).stem
            subprocess.run([*shlex.split(compiler), standard, '-Wall', '-Wextra', '-Werror', '-O2',
                            *includes, '-o', str(binary), str(tests / source)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)
    print('QUERY_POLICY PASS: accounting waits for completion; ordinary renders remain deferred; unsupported statistics are not advertised')


if __name__ == '__main__':
    main()
