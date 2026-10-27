"""Independent integer/checksum/accounting checks of the actual driver JSON."""
import argparse
import json
import math
from pathlib import Path
import subprocess

MASK = (1 << 64) - 1
BACKENDS = {'new_delete_resource', 'bump_arena', 'fixed_pool', 'pmr_monotonic', 'pmr_pool'}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def counters(c):
    require(c['allocation_requests'] == c['allocations'] + c['failures'], 'request/success/failure accounting')
    require(c['allocated_bytes'] - c['deallocated_bytes'] == c['current_bytes'] == 0, 'final live counter')
    require(c['allocations'] == c['deallocations'], 'final allocation count')
    require(c['allocated_bytes'] >= c['peak_bytes'] >= 0, 'peak bytes')
    require(sum(a['count'] for a in c['successful_alignment_counts']) == c['allocations'], 'alignment counts')
    for a in c['successful_alignment_counts']:
        require(a['alignment'] > 0 and a['alignment'] & (a['alignment'] - 1) == 0, 'alignment is power of two')


def validate_document(data):
    require(data['schema'] == 'memory-lab-v1' and data['fixture_ok'], 'schema/fixture status')
    config = data['config']
    n, seed, reps = config['size'], config['seed'], config['repetitions']
    require(1 <= n <= 2048 and 0 <= seed <= MASK and 1 <= reps <= 20, 'bounded config')
    layouts = data['layout']['fixture_layouts']
    require(len(layouts) == 3, 'all layout fixtures present')
    fields = []
    for layout in layouts:
        size = layout['object_size']
        require(size == layout['array_stride'] and size % layout['object_alignment'] == 0, 'layout stride/alignment')
        owners = [-1] * size
        end = max(m['offset'] + m['size'] for m in layout['members'])
        owners[end:] = [-2] * (size - end)
        for i, member in enumerate(layout['members']):
            offset, width = member['offset'], member['size']
            require(offset >= 0 and width > 0 and offset + width <= size, 'member extent')
            require(offset % member['alignment'] == 0, 'member alignment')
            for b in range(offset, offset + width):
                require(owners[b] < 0, 'overlapping member')
                owners[b] = i
        require(owners == layout['byte_owner'], 'computed byte map')
        require(owners.count(-1) == layout['internal_padding'] and owners.count(-2) == layout['tail_padding'], 'padding totals')
        coverage = [0] * size
        for gap in layout['padding']:
            require(gap['size'] > 0 and 0 <= gap['offset'] < size and gap['offset'] + gap['size'] <= size, 'padding extent')
            for b in range(gap['offset'], gap['offset'] + gap['size']):
                require(owners[b] == (-2 if gap['tail'] else -1), 'padding kind')
                coverage[b] += 1
        require(coverage == [int(owner < 0) for owner in owners], 'every padding byte exactly once')
        fields.append(sorted((m['name'], m['size'], m['alignment']) for m in layout['members']))
    require(fields[0] == fields[1] == fields[2], 'same named scalar fields')
    arena = data['arena']
    require(arena['consumed'] == arena['payload_bytes'] + arena['alignment_padding'], 'arena payload/padding')
    require(arena['capacity'] == arena['consumed'] + arena['remaining'], 'arena capacity')
    require(arena['consumed_after_reset'] == 0 and arena['huge_failure_preserves_state'], 'arena reset/failure')
    require(arena['first_alignment_remainder'] == arena['second_alignment_remainder'] == 0, 'aligned arena blocks')
    pool = data['fixed_pool']
    require(pool['alignment_padding'] + pool['capacity'] * pool['slot_stride'] + pool['tail_bytes'] == pool['backing_capacity'], 'pool backing accounting')
    require(pool['live_count'] == 0 and pool['free_count'] == pool['capacity'] and pool['release_reuse_ok'], 'pool final/reuse')
    require(pool['metadata_used_bytes'] <= pool['metadata_reserved_bytes'], 'metadata bound')
    for name in ('monotonic_fallback', 'monotonic_hardcap', 'pool'):
        route = data['pmr'][name]
        require(route['fixture_ok'] and route['constructed'] == route['destroyed'] > 0, 'PMR lifetime/values')
        require(route['final_upstream_bytes'] == route['upstream_after_release'] == 0, 'PMR final upstream')
        counters(route['frontend'])
        counters(route['upstream'])
    fallback, hard = data['pmr']['monotonic_fallback'], data['pmr']['monotonic_hardcap']
    require(not fallback['bad_alloc_observed'] and fallback['upstream']['allocations'] > 0, 'explicit fallback observed')
    require(fallback['upstream_before_block_free'] == fallback['upstream_after_block_free'] > 0, 'monotonic per-free retention')
    require(hard['bad_alloc_observed'] and hard['upstream']['allocations'] == 0 and hard['upstream']['failures'] > 0, 'null upstream hard cap')
    require(hard['frontend']['allocations'] > 0, 'initial-buffer route visible only at frontend')
    require(all(data['pmr']['copy_move'].values()), 'copy/move resource selection')
    experiment = data['experiments']
    require(all(experiment[k] == config[k] for k in config), 'experiment config')
    state = seed ^ 0x9e3779b97f4a7c15
    trace = []
    for _ in range(n):
        state = (state ^ (state << 13)) & MASK
        state ^= state >> 7
        state = (state ^ (state << 17)) & MASK
        trace.append(experiment['object_size'] + state % (65 - experiment['object_size']))
    require(experiment['trace_bytes'] == trace, 'independent runtime trace')
    payload = sum(trace)
    raw_sum = sum((seed + i * 17 + j * 3) & 255 for i, width in enumerate(trace) for j in range(width)) & MASK
    object_sum = sum(seed + i * 0x100000001b3 for i in range(n)) & MASK
    require({b['name'] for b in experiment['backends']} == BACKENDS and len(experiment['backends']) == 5, 'all equivalent backends')
    for backend in experiment['backends']:
        name = backend['name']
        require(backend['warmup_ns'] >= 0 and len(backend['samples']) == reps, 'warmup/repetitions')
        for index, sample in enumerate(backend['samples']):
            require(sample['repetition'] == index, 'repetition identity')
            for phase_name, expected in (('raw', raw_sum), ('objects', object_sum)):
                phase = sample[phase_name]
                require(phase['fixture_ok'] and phase['checksum'] == phase['expected_checksum'] == expected, 'independent payload/object checksum')
                require(phase['allocations'] == n and phase['payload_bytes'] == payload, 'equivalent counts/payload')
                require(phase['constructed'] == phase['destroyed'] == (n if phase_name == 'objects' else 0), 'object destruction')
                timers = phase['timings_ns']
                require(all(t is None or (math.isfinite(t) and t >= 0) for t in timers.values()), 'finite nonnegative raw times')
                require((timers['construct'] is None) == (phase_name == 'raw'), 'separate construction timing')
                rate = phase['allocation_operations_per_second']
                if timers['allocate'] > 0:
                    require(math.isclose(rate, n * 1e9 / timers['allocate'], rel_tol=1e-12), 'fractional rate/units')
                else:
                    require(rate is None and not phase['timing_valid'] and phase['errors'], 'zero clock policy')
                storage = phase['storage_bytes']
                require(storage['requested_live_payload'] == payload and storage['final_upstream_bytes'] == 0, 'storage payload/final upstream')
                require(storage['retained_not_live_after_deallocate'] == storage['backing_capacity'] + storage['upstream_after_deallocate'], 'retention definition')
                require(storage['host_allocator_hidden_bytes'] is None and storage['pmr_cursor_or_internal_metadata'] is None, 'unknowns not fabricated')
                counters(phase['upstream'])
                if name == 'bump_arena':
                    require(phase['deallocations'] == 0 and timers['deallocate'] is None and timers['release_or_reset'] is not None, 'arena unsupported free/collective reset')
                    require(storage['consumed'] == payload + storage['alignment_slack'], 'measured arena cursor')
                else:
                    require(phase['deallocations'] == n and timers['deallocate'] is not None, 'matched deallocations')
                if name in ('bump_arena', 'fixed_pool'):
                    require(phase['frontend'] is None and phase['frontend_null_reason'], 'no fictitious PMR frontend')
                else:
                    counters(phase['frontend'])
                    require(phase['frontend']['allocations'] == n, 'observed frontend requests')
                if name == 'fixed_pool':
                    require(storage['consumed'] == n * 64 and storage['slot_rounding'] == n * 64 - payload, 'fixed-slot internal rounding')
                if name.startswith('pmr_'):
                    require(storage['consumed'] is None and storage['metadata'] is None, 'opaque PMR cursor/metadata')
    model = experiment['fragmentation']
    require('model' in model['kind'] and 'not libc' in model['kind'], 'model label')
    holes = model['fragmented_holes']
    require(sum(h['size'] for h in holes) == model['fragmented_total_free'] == 32, 'model total free')
    require(max(h['size'] for h in holes) == model['fragmented_largest_free'] == 16, 'model largest free')
    require(model['fragmented_largest_free'] < model['failed_request'] <= model['fragmented_total_free'], 'fragmentation despite sufficient total')
    require(model['fragmented_failed'] and model['contiguous_succeeded'] and model['after_merge_succeeded'], 'fragmented/contiguous comparison')
    require(model['before_merge_total'] == model['after_merge_total'] == 48 and model['before_merge_largest'] == 16 and model['after_merge_largest'] == 48, 'explicit adjacent coalescing')
    return config


def main():
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--binary', type=Path)
    group.add_argument('--json', type=Path)
    args = parser.parse_args()
    if args.json:
        validate_document(json.loads(args.json.read_text()))
    else:
        for n, seed, reps in ((1, 0, 1), (37, MASK, 2), (2048, 42, 1)):
            result = subprocess.run([str(args.binary), '--size', str(n), '--seed', str(seed), '--repetitions', str(reps)], capture_output=True, text=True, timeout=25)
            require(result.returncode == 0, result.stderr)
            require(validate_document(json.loads(result.stdout)) == {'size': n, 'seed': seed, 'repetitions': reps}, 'CLI config identity')
        for options in (['--size', '0'], ['--size', '2049'], ['--size', '-1'], ['--seed', str(MASK + 1)], ['--repetitions', '21'], ['--repetitions', '0'], ['--size'], ['--unknown', '1'], ['--size', '1', '--size', '2']):
            result = subprocess.run([str(args.binary), *options], capture_output=True, text=True, timeout=10)
            require(result.returncode == 2 and not result.stdout and bool(result.stderr), 'invalid CLI did not fail safely')
    print('passed: independent memory driver trace/checksums, layouts, lifetimes, accounting and fragmentation')


if __name__ == '__main__':
    main()
