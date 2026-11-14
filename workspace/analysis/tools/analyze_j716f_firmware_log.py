"""Read-only validation of one J716FDIAG v1 QR log; not an EBS/boot validator."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def coverage(start, end, descriptors):
    """Compute the union coverage, rather than equating an overlap to coverage."""
    cursor = start
    gaps, hits = [], []
    for item in sorted(descriptors, key=lambda d: d['start']):
        lo, hi = max(start, item['start']), min(end, item['end'])
        if hi <= lo:
            continue
        if lo > cursor:
            gaps.append([cursor, lo])
        cursor = max(cursor, hi)
        hits.append({'index': item['index'], 'type': item['type'],
                     'type_name': item['type_name'], 'intersection': [lo, hi]})
    if cursor < end:
        gaps.append([cursor, end])
    return {'fully_covered': end > start and not gaps, 'gaps': gaps,
            'descriptors': hits, 'types': sorted({d['type'] for d in hits})}


def analyze(raw):
    text = raw.decode('utf-8', errors='strict')
    lines = text.splitlines()
    errors = []

    def require(ok, message):
        if not ok:
            errors.append(message)

    def occurrences(token):
        return [n for n, line in enumerate(lines, 1) if token in line]

    markers = {name: occurrences(token) for name, token in {
        'registration': 'J716FDIAG ReadyToBoot registration Success',
        'begin': 'J716FDIAG BEGIN v1 phase=ReadyToBoot-before-UniXNU',
        'end': 'J716FDIAG END snapshot;',
        'start_unixnu_application': 'J716F V11 START_UNIXNU',
    }.items()}
    for name, found in markers.items():
        require(len(found) == 1, f'Expected one {name} marker; found {len(found)}')
    if all(len(found) == 1 for found in markers.values()):
        require(markers['registration'][0] < markers['begin'][0] <
                markers['end'][0] < markers['start_unixnu_application'][0],
                'Diagnostic marker order is invalid')
    # Multiple snapshots are deliberately rejected instead of silently merged.
    begin, end = markers['begin'], markers['end']
    block = lines[begin[0]-1:end[0]] if len(begin) == len(end) == 1 and begin[0] < end[0] else []
    snapshot = '\n'.join(block)
    headers = re.findall(r'J716FDIAG MAP key=(0x[0-9A-Fa-f]+) bytes=(0x[0-9A-Fa-f]+) stride=(\d+) version=(\d+) count=(\d+)', snapshot)
    require(len(headers) == 1, 'Expected exactly one map header')
    header = None
    if len(headers) == 1:
        key, size, stride, version, count = headers[0]
        header = {'key': int(key, 16), 'bytes': int(size, 16), 'stride': int(stride),
                  'version': int(version), 'count': int(count)}
        require(header['stride'] >= 40, 'Descriptor stride smaller than EFI descriptor')
        require(header['bytes'] == header['stride'] * header['count'], 'Map size/count mismatch')

    descriptor_pattern = re.compile(r'J716FDIAG MAP\[(\d+)\] type=(\d+)/(\w+) \[(0x[0-9A-Fa-f]+),(0x[0-9A-Fa-f]+)\) pages=(0x[0-9A-Fa-f]+) attr=(0x[0-9A-Fa-f]+) virt=(0x[0-9A-Fa-f]+)')
    descriptors = []
    for match in descriptor_pattern.finditer(snapshot):
        index, kind, name, lo, hi, pages, attrs, virt = match.groups()
        item = {'index': int(index), 'type': int(kind), 'type_name': name,
                'start': int(lo, 16), 'end': int(hi, 16), 'pages': int(pages, 16),
                'attributes': int(attrs, 16), 'virtual': int(virt, 16)}
        require(item['pages'] > 0 and item['end'] <= 2**64 and
                item['end'] - item['start'] == item['pages'] * 4096,
                f'MAP[{index}] invalid page count/range')
        require(item['start'] % 4096 == 0 and item['end'] % 4096 == 0,
                f'MAP[{index}] not page aligned')
        descriptors.append(item)
    if header:
        require([d['index'] for d in descriptors] == list(range(header['count'])),
                'Missing, repeated or unordered descriptor indices')
    sorted_map = sorted(descriptors, key=lambda d: (d['start'], d['end']))
    overlaps = []
    for n, item in enumerate(sorted_map):
        for other in sorted_map[n+1:]:
            if other['start'] >= item['end']:
                break
            overlaps.append([item['index'], other['index']])
    require(not overlaps, 'EFI memory descriptors overlap')
    holes = [[a['end'], b['start']] for a, b in zip(sorted_map, sorted_map[1:]) if a['end'] < b['start']]

    ranges = {}
    for name, lo, hi in re.findall(r'J716FDIAG RANGE ([\w-]+) \[(0x[0-9A-Fa-f]+),(0x[0-9A-Fa-f]+)\)', snapshot):
        require(name not in ranges, f'Duplicate range {name}')
        lo, hi = int(lo, 16), int(hi, 16)
        ranges[name] = {'start': lo, 'end': hi, 'bytes': hi-lo, **coverage(lo, hi, descriptors)}
        require(ranges[name]['fully_covered'], f'Incomplete map coverage of {name}')
    required_ranges = ['diagnostic-map-buffer', 'system-table', 'gop-framebuffer', 'advanced-logger', 'incoming-fdt']
    for name in required_ranges:
        require(name in ranges, f'Missing range {name}')
    live_reservations = {}
    for name in ['gop-framebuffer', 'advanced-logger', 'incoming-fdt', 'incoming-initrd']:
        if name in ranges:
            live_reservations[name] = ranges[name]['fully_covered'] and ranges[name]['types'] == [0]
    conflicts = [line for line in block if 'CONFLICT' in line]
    range_collisions = []
    pairs = list(ranges.items())
    for i, (left, a) in enumerate(pairs):
        for right, b in pairs[i+1:]:
            if max(a['start'], b['start']) < min(a['end'], b['end']):
                range_collisions.append([left, right])
    initrds = re.findall(r'J716FDIAG INITRD bytes=(0x[0-9A-Fa-f]+) sector-remainder=(\d+)', snapshot)
    initrd = None
    if 'incoming-initrd' in ranges:
        require(len(initrds) == 1, 'Missing or repeated initrd size')
        if len(initrds) == 1:
            initrd = {'bytes': int(initrds[0][0],16), 'sector_remainder': int(initrds[0][1])}
            require(initrd['bytes'] == ranges['incoming-initrd']['bytes'], 'Initrd length differs from its range')
            require(initrd['bytes'] % 512 == initrd['sector_remainder'], 'Initrd sector remainder mismatch')

    gops = re.findall(r'J716FDIAG GOP mode=(\d+) max=(\d+) width=(\d+) height=(\d+) stride=(\d+) format=(\d+)', snapshot)
    gop = dict(zip(['mode','max','width','height','stride','format'], map(int, gops[0]))) if len(gops) == 1 else None
    require(gop is not None, 'Missing or repeated GOP geometry')
    if gop and 'gop-framebuffer' in ranges:
        require(gop['stride'] >= gop['width'] > 0 and gop['height'] > 0, 'Invalid GOP geometry')
        if gop['format'] in (0, 1):
            gop['pixel_storage_bytes'] = gop['stride'] * gop['height'] * 4
            require(ranges['gop-framebuffer']['bytes'] >= gop['pixel_storage_bytes'], 'GOP buffer too small')
    loggers = re.findall(r'J716FDIAG LOGGER version=(\d+) current-offset=(0x[0-9A-Fa-f]+) discarded=(0x[0-9A-Fa-f]+)', snapshot)
    logger = {'version': int(loggers[0][0]), 'current_offset': int(loggers[0][1],16), 'discarded': int(loggers[0][2],16)} if len(loggers) == 1 else None
    require(logger is not None, 'Missing or repeated logger state')
    fdt = re.findall(r'J716FDIAG FDT incoming-x0=(0x[0-9A-Fa-f]+)', snapshot)
    if len(fdt) == 1 and 'incoming-fdt' in ranges:
        require(int(fdt[0],16) == ranges['incoming-fdt']['start'], 'FDT x0 differs from range start')
    else:
        require(False, 'Missing or repeated FDT x0')
    require('\x00' not in text, 'NUL byte in text export')
    require(raw.endswith(b'\n'), 'Last exported line is not terminated')
    limitations = [
        'Only a ReadyToBoot snapshot, not the final ExitBootServices map or an XNU ownership grant.',
        'discarded is measured at the LOGGER record, not at QR capture completion.',
        'Absence of SEC logs does not prove their execution or loss; this export begins in DXE.',
        'Image identity comes from the capture session; the log has no boot.img hash.',
        'Map holes are reported without classifying them as RAM, allocatable space or faults.'
    ]
    hob = re.findall(r'HOBLIST address in DXE = (0x[0-9A-Fa-f]+)', text)
    if 'hob-list' not in ranges:
        limitations.append('No bounded hob-list range was reported; HOB list length/coverage remains unverified.')
    return {
        'schema': 'j716f-log-audit-v1', 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
        'line_count': len(lines), 'first_line': lines[0] if lines else '', 'last_line': lines[-1] if lines else '',
        'diagnostic_structure_valid': not errors, 'errors': errors, 'markers': markers,
        'map': {'header': header, 'descriptors': descriptors, 'overlaps': overlaps, 'holes': holes},
        'ranges': ranges, 'live_ranges_fully_reserved': live_reservations, 'conflict_lines': conflicts,
        'reported_range_collisions': range_collisions, 'initrd': initrd,
        'gop': gop, 'logger_at_snapshot': logger,
        'hob_start_reported_by_dxe': [int(v,16) for v in hob], 'hob_full_range_verified': 'hob-list' in ranges,
        'initrd_absent_or_invalid': 'J716FDIAG INITRD absent/invalid address properties' in snapshot,
        'builtin_configuration': occurrences('config: none found, using built-in configuration'),
        'external_configuration_loaded_lines': occurrences('UNIXNU: config: loaded '),
        'ramdisk_block_device_lines': occurrences('UNIXNURAMDISK: initrd block device '),
        'menu_hold': occurrences('menu: diagnostic hold enabled;'),
        'darwinboot_log_lines': occurrences('DARWINBOOT:'),
        'error_and_warning_lines': [{'line': n, 'text': line} for n,line in enumerate(lines,1) if re.search(r'\[(ERR |WARN)\]',line)],
        'memory_protection_hob_missing_lines': occurrences('Unable to fetch memory protection HOB'),
        'limitations': limitations,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.log.resolve() == args.output.resolve():
        parser.error('Output cannot replace the original log')
    result = analyze(args.log.read_bytes())
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: result[key] for key in ['bytes','sha256','line_count','diagnostic_structure_valid','errors','gop','logger_at_snapshot','live_ranges_fully_reserved','hob_full_range_verified']}, indent=2))
    raise SystemExit(0 if result['diagnostic_structure_valid'] else 1)
