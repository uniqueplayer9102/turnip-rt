import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name, cpp=False):
    prefix = r'^static bool ' if cpp else (
        r'^(?:VKAPI_ATTR void VKAPI_CALL|(?:static (?:inline )?)?'
        r'(?:void|bool|unsigned|uint32_t|uint64_t|VkResult|VkFormat|VkExtent3D|VkImageAspectFlags|'
        r'VkImageUsageFlags2KHR|nir_mem_access_size_align|'
        r'const struct (?:rd_binding|rd_nir_resource_binding|vk_sampler_state|'
        r'vk_ycbcr_conversion_state|vk_format_ycbcr_info) \*))\n')
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
    command = (driver / 'rd_command.c').read_text()
    image_src = (driver / 'rd_image.c').read_text()
    device_src = (driver / 'rd_device.c').read_text()
    private = (driver / 'rd_private.h').read_text()
    event = (driver / 'rd_event.c').read_text()
    query = (driver / 'rd_query.c').read_text()
    vk_runtime = driver.parent.parent / 'vulkan' / 'runtime'
    sync_header = (vk_runtime / 'vk_sync.h').read_text()
    direct = extract(graphics, 'rd_execute_render_direct')
    assert re.search(r'result\s*=\s*queries\s*\?\s*rd_finish_submit\(', direct)
    assert direct.index('result = queries ?') < direct.index('if (result == VK_SUCCESS && queries)')
    assert 'q.zero |= i != 0;' in graphics
    assert '.pipelineStatisticsQuery = false' in instance
    assert '#ifdef VK_USE_PLATFORM_XCB_KHR' in instance
    assert '.GOOGLE_display_timing = true,' in instance
    assert '.KHR_calibrated_timestamps = true,' in instance
    assert 'vk_device_dispatch_table_from_entrypoints(&dispatch, &vk_common_device_entrypoints, false)' in device_src
    assert 'device->vk.get_timestamp = rd_device_get_timestamp;' in device_src
    assert 'vk_physical_device_dispatch_table_from_entrypoints(&dispatch,' in instance
    assert '&vk_common_physical_device_entrypoints, false)' in instance
    assert '.KHR_xcb_surface = true,' in instance
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
    diag_report = extract(queue, 'rd_submit_diag_report')
    assert 'RADISH_SUBMIT_DIAG id=' in diag_report and 'seq=' in diag_report
    assert 'RADISH_SUBMIT_DIAG_EVERY' in queue
    for field in ('seq_before', 'cmd_wall_ns', 'cmd_host_ns', 'submit_ready_ns',
                  'wait_export_ns', 'completion_wait_ns', 'oldest_wait_ns',
                  'wait_export_result', 'out_fence_export_failures'):
        assert field + '=' in diag_report, field
    assert 'vk_id=' in queue
    assert 'RADISH_PENDING_BLOCKING_WAIT' in device_src
    assert '!!transfer->src_buffer == !!transfer->dst_buffer' in image_src
    assert 'assert(!!transfer->src_buffer != !!transfer->src_image)' in image_src
    assert 'compute_deferred' not in queue
    assert 'rd_compute_can_defer(async, deferred,' in queue
    assert 'device->publish_signal_fence' in queue
    assert 'RADISH_RENDER_BATCH' in queue
    assert 'RADISH_RENDER_BATCH_DEBUG' in queue
    assert 'rd_render_batch_count' in queue
    assert 'rd_render_batch_supported' in graphics
    assert 'rd_execute_render_batch' in graphics
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
        image_h = (vk_runtime / 'vk_image.h').read_text()
        vk_util = vk_runtime.parent / 'util'
        format_h = (vk_util / 'vk_format.h').read_text()
        format_c = (vk_util / 'vk_format.c').read_text()
        util_h = (vk_util / 'vk_util.h').read_text()
        enum_helpers = util_h.index('#define VK_EXT_OFFSET')
        (directory / 'ycbcr_formats_under_test.h').write_text(
            util_h[enum_helpers:util_h.index('\n\n', enum_helpers)] + '\n' +
            format_h[format_h.index('struct vk_format_ycbcr_plane {'):
                     format_h.index('const struct vk_format_ycbcr_info *vk_format_get_ycbcr_info')] + '\n' +
            format_c[format_c.index('#define fmt_unsupported'):
                     format_c.index('const struct vk_format_ycbcr_info *\nvk_format_get_ycbcr_info')] +
            ''.join(extract(format_c, name) for name in ('vk_format_get_ycbcr_info', 'vk_format_aspects')) +
            extract(format_h, 'vk_format_get_plane_count') + extract(format_c, 'vk_format_get_plane_format') +
            extract(image_h, 'vk_image_mip_level_extent') + ''.join(
                extract(image_src, name) for name in ('rd_ycbcr_format_supported',
                                                     'rd_image_aspect_plane', 'rd_image_plane_extent')))
        nir_resources = (driver / 'rd_nir_resources.c').read_text()
        (directory / 'sampler_slots_under_test.h').write_text(
            extract(pipeline, 'rd_pipeline_resource') + extract(nir_resources, 'rd_mark_resource') +
            extract(nir_resources, 'rd_lower_texture_sampler'))
        (directory / 'sampled_textures_under_test.h').write_text(''.join(
            extract(resources, name) for name in ('rd_resource_packet', 'rd_emit_resource_texture_words',
                                                   'rd_emit_resource_texture_sb', 'rd_emit_sampled_textures')))
        layers = image_h.index('#define vk_image_subresource_layer_count(')
        (directory / 'clear_ds_under_test.h').write_text(
            image_h[layers:image_h.index('\n\n', layers)] + '\n' +
            extract(image_h, 'vk_image_subresource_level_count') +
            extract((vk_runtime / 'vk_image.c').read_text(), 'vk_image_usage') +
            extract(graphics, 'rd_clear_ds_range_valid') +
            extract(graphics, 'rd_CmdClearDepthStencilImage'))
        (directory / 'clear_ds_types_under_test.h').write_text(
            private[private.index('enum rd_command_type {'):private.index('struct rd_transfer {')])
        (directory / 'sync_waits_under_test.h').write_text(
            extract(sync_header, 'vk_sync_waits_have_timeline'))
        (directory / 'sync_policy_under_test.h').write_text(''.join(
            extract(queue, name) for name in ('rd_compute_can_defer', 'rd_publish_signal_fences',
                                              'rd_compute_tail_batch',
                                              'rd_render_exports_fence',
                                              'rd_render_query_before_work')))
        (directory / 'vertex_input_under_test.h').write_text(
            extract(graphics, 'rd_vertex_input_valid'))
        (directory / 'query_range_under_test.h').write_text(
            extract(query, 'rd_query_range'))
        (directory / 'push_constants_under_test.h').write_text(
            extract(command, 'rd_push_constants_covered') +
            extract(command, 'rd_CmdPushConstants'))
        set_version = private[private.index('struct rd_descriptor_set_version {'):
                              private.index('struct rd_descriptor_set {')]
        (directory / 'descriptor_set_version_under_test.h').write_text(set_version)
        conversion_h = (vk_runtime / 'vk_ycbcr_conversion.h').read_text()
        conversion = conversion_h.index('struct vk_ycbcr_conversion_state {')
        (directory / 'ycbcr_conversion_state_under_test.h').write_text(
            conversion_h[conversion:conversion_h.index('\n};', conversion) + 3] + '\n')
        (directory / 'descriptor_updates_under_test.h').write_text(
            extract((vk_runtime / 'vk_buffer.h').read_text(), 'vk_buffer_range') + ''.join(
                extract(descriptor, name) for name in (
                    'rd_descriptor_type_supported', 'rd_descriptor_layout_supported',
                    'rd_find_binding', 'rd_descriptor_span_valid', 'rd_immutable_sampler',
                    'rd_descriptor_set_init_version', 'rd_sampler_ycbcr_conversion',
                    'rd_ycbcr_conversions_match', 'rd_validate_descriptor_conversion',
                    'rd_write_descriptor', 'rd_UpdateDescriptorSets',
                    'rd_descriptor_template_data_valid', 'rd_UpdateDescriptorSetWithTemplate')))
        (directory / 'dispatch_preflight_under_test.h').write_text(
            extract(descriptor, 'rd_descriptor_dynamic_offset_valid') +
            ''.join(extract(queue, name) for name in ('rd_window_pipeline',
                                                     'rd_resolve_dispatch_descriptors',
                                                     'rd_window_dispatch_supported')))
        cache_types = private[private.index('struct rd_stage_dynamic_offsets {'):
                              private.index('struct rd_command_buffer {')]
        (directory / 'draw_descriptor_cache_types_under_test.h').write_text(cache_types)
        (directory / 'draw_descriptor_cache_under_test.h').write_text(
            extract(private, 'rd_invalidate_draw_descriptor_cache') +
            extract(graphics, 'rd_draw_descriptor_cache_track_set') +
            extract(graphics, 'rd_draw_descriptor_cache_enabled') +
            extract(graphics, 'rd_draw_descriptor_cache_hit'))
        diag_types = private[private.index('enum rd_drain_reason {'):private.index('\nstruct rd_device {')]
        drain_names = queue[queue.index('static const char *rd_drain_names'):queue.index('\nstatic unsigned\nrd_drain_index')]
        (directory / 'submit_diag_under_test.h').write_text(diag_types + drain_names + '\n' + ''.join(
            extract(queue, name) for name in ('rd_drain_index', 'rd_submit_diag_accounted_ns',
                                               'rd_submit_diag_end_commands')))
        (directory / 'copy_side_under_test.h').write_text(''.join(
            extract(image_src, name) for name in ('rd_image_transfer_memory',
                                                  'rd_image_copy_side', 'rd_image_copy_bos')))
        (directory / 'ring_backings_under_test.h').write_text(
            extract(queue, 'rd_ring_backings'))
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
