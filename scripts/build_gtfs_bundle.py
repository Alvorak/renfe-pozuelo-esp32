#!/usr/bin/env python3
"""US-07: enlarge both ESP32 GTFS banks consistently to 3 MiB.

Run from renfe-pozuelo-esp32 repository root:
    py US07_fix_bancos_3MiB.py --check
    py US07_fix_bancos_3MiB.py --apply

Changes only partitions.csv, main/gtfs_auto.c, and
scripts/build_gtfs_bundle.py. A backup is written beside the repository.
"""
import argparse
import csv
from datetime import datetime
from io import StringIO
from pathlib import Path
import re
import shutil
import sys

OLD = 0x180000
NEW = 0x300000
FLASH_16MB = 0x1000000
PATHS = ('partitions.csv', 'main/gtfs_auto.c', 'scripts/build_gtfs_bundle.py')

OLD_MAP = '''        const void *p = NULL;
        esp_partition_mmap_handle_t handle;
        if (esp_partition_mmap(slots[i], 0, SLOT_BYTES,
                               ESP_PARTITION_MMAP_DATA, &p, &handle) != ESP_OK) continue;
        bundle_info_t info;
        if (validate_payload((const uint8_t *)p, SLOT_BYTES, &info) &&'''
NEW_MAP = '''        // Read the small header first: do not reserve a whole 3 MiB MMU mapping.
        // An uninitialized or interrupted bank fails this check harmlessly.
        uint8_t header[HDR_SIZE];
        if (esp_partition_read(slots[i], 0, header, sizeof(header)) != ESP_OK) continue;
        bundle_info_t declared;
        if (!inspect_header(header, slots[i]->size, &declared)) continue;
        const size_t bundle_size = HDR_SIZE + (size_t)declared.primary_len + declared.upstream_len;
        const void *p = NULL;
        esp_partition_mmap_handle_t handle;
        if (esp_partition_mmap(slots[i], 0, bundle_size,
                               ESP_PARTITION_MMAP_DATA, &p, &handle) != ESP_OK) continue;
        bundle_info_t info;
        if (validate_payload((const uint8_t *)p, bundle_size, &info) &&'''


def exactly_once(content, old, new, label):
    if content.count(old) != 1:
        raise ValueError(f'{label}: expected exactly one anchor; got {content.count(old)}')
    return content.replace(old, new, 1)


def parse_partitions(content):
    result = []
    for line in content.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields = next(csv.reader(StringIO(line)))
        if len(fields) < 5:
            raise ValueError(f'Invalid partitions.csv line: {line!r}')
        result.append([x.strip() for x in fields[:5]])
    return result


def part_size(raw):
    raw = raw.strip().upper()
    if raw.endswith('M'):
        return int(raw[:-1], 0) * 1024 * 1024
    if raw.endswith('K'):
        return int(raw[:-1], 0) * 1024
    return int(raw, 0)


def layout_check(content):
    parts = parse_partitions(content)
    seen = set()
    previous_end = 0x9000
    layout = []
    for name, typ, subtype, off, size_raw in parts:
        if name in seen:
            raise ValueError(f'Duplicate partition: {name}')
        seen.add(name)
        size = part_size(size_raw)
        align = 0x10000 if typ == 'app' else 0x1000
        start = int(off, 0) if off else ((previous_end + align - 1) // align) * align
        end = start + size
        if start < previous_end or start % align:
            raise ValueError(f'Overlapping/unaligned partition: {name} at 0x{start:x}')
        if end > FLASH_16MB:
            raise ValueError(f'Partitions exceed 16 MiB flash: {name} ends 0x{end:x}')
        previous_end = end
        layout.append((name, start, end, size))
    by_name = {n:(s,e,z) for n,s,e,z in layout}
    if 'factory' not in by_name or by_name['factory'][:2] != (0x10000, 0x410000):
        raise ValueError('Factory app offset/size differ from known stable 4 MiB layout; refusing')
    if 'nvs' not in by_name or by_name['nvs'][0] != 0x9000:
        raise ValueError('NVS no longer starts at 0x9000; refusing')
    for name in ('gtfs_a', 'gtfs_b'):
        if name not in by_name or by_name[name][2] != NEW:
            raise ValueError(f'{name} is missing or is not 3 MiB')
    if by_name['gtfs_a'][:2] != (0x410000, 0x710000) or by_name['gtfs_b'][:2] != (0x710000, 0xA10000):
        raise ValueError('Unexpected GTFS offsets; refusing')
    return layout


def change_partitions(original):
    updated = original
    for slot, sub in (('gtfs_a', '0x40'), ('gtfs_b', '0x41')):
        pattern = rf'(?m)^({slot}\s*,\s*data\s*,\s*{sub}\s*,\s*,\s*)0x180000(\s*,)'
        updated, matches = re.subn(pattern, r'\g<1>0x300000\g<2>', updated)
        if matches != 1:
            raise ValueError(f'{slot}: expected original 1.5 MiB partition with auto offset; got {matches}')
    layout = layout_check(updated)
    return updated, layout


def change_firmware(original):
    updated = exactly_once(original, '#define SLOT_BYTES (0x180000U)',
                           '#define SLOT_BYTES (0x300000U)', 'firmware SLOT_BYTES')
    updated = exactly_once(updated, OLD_MAP, NEW_MAP, 'firmware startup mmap')
    return updated


def change_builder(original):
    updated = exactly_once(original, 'if len(packed) > 0x180000:',
                           'if len(packed) > 0x300000:', 'builder size check')
    updated = exactly_once(updated, 'exceeds 1.5MiB partition',
                           'exceeds 3MiB partition', 'builder error message')
    return updated


def prepare(root):
    files = {name: (root / name).read_text(encoding='utf-8') for name in PATHS}
    part, layout = change_partitions(files['partitions.csv'])
    new = {'partitions.csv':part,
           'main/gtfs_auto.c':change_firmware(files['main/gtfs_auto.c']),
           'scripts/build_gtfs_bundle.py':change_builder(files['scripts/build_gtfs_bundle.py'])}
    config = root / 'sdkconfig'
    if config.is_file():
        cfg = config.read_text(encoding='utf-8')
        if not (re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y$', cfg, re.M) or
                re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE="16MB"$', cfg, re.M)):
            raise ValueError('sdkconfig does not declare 16MB flash; check board/flash before applying')
    else:
        print('WARNING: no sdkconfig found; physically verify the ESP32-S3 has 16MB flash')
    return files, new, layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--check', action='store_true')
    group.add_argument('--apply', action='store_true')
    a = parser.parse_args()
    root = Path.cwd().resolve()
    if not (root / 'main').is_dir() or not (root / 'scripts').is_dir():
        print('ERROR: run this from the renfe-pozuelo-esp32 project root', file=sys.stderr)
        return 1
    try:
        # Re-running the installer is a no-op only when ALL three changes
        # are already present; mixed states are rejected for safety.
        existing = {name: (root/name).read_text(encoding='utf-8') for name in PATHS}
        state = [
            'gtfs_a, data, 0x40, , 0x300000' in existing['partitions.csv']
                and 'gtfs_b, data, 0x41, , 0x300000' in existing['partitions.csv'],
            '#define SLOT_BYTES (0x300000U)' in existing['main/gtfs_auto.c']
                and 'esp_partition_read(slots[i], 0, header, sizeof(header))' in existing['main/gtfs_auto.c'],
            'if len(packed) > 0x300000:' in existing['scripts/build_gtfs_bundle.py']
        ]
        if all(state):
            layout_check(existing['partitions.csv'])
            print('US-07 BANCOS 3 MiB: ALREADY INSTALLED - no modifications needed')
            return 0
        if any(state):
            raise ValueError('Partial 3 MiB upgrade detected; review the three files before proceeding')
        original, updated, layout = prepare(root)
        print('GTFS bundle from workflow: 2,195,860 bytes')
        print('New GTFS slot size: 3,145,728 bytes (3 MiB each)')
        for name, start, end, size in layout:
            print(f'  {name:10s} 0x{start:07x} - 0x{end:07x} ({size} bytes)')
        if a.check:
            print('\nCHECK OK - no files modified')
            return 0
        backup = root.parent / (root.name + '_backup_US07_3MiB_' + datetime.now().strftime('%Y%m%d_%H%M%S'))
        backup.mkdir(exist_ok=False)
        for name in PATHS:
            path = backup / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(original[name], encoding='utf-8')
        for name in PATHS:
            (root/name).write_text(updated[name], encoding='utf-8')
        print(f'\nAPPLY OK - backup at {backup}')
        print('Next: py -m py_compile scripts/build_gtfs_bundle.py, git diff, idf.py build')
        return 0
    except (OSError, ValueError, UnicodeError) as exc:
        print(f'ABORTED without applying changes: {type(exc).__name__}: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
