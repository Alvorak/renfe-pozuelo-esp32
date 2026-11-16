#!/usr/bin/env python3
"""Build a compact, ESP32-readable two-index bundle from a Renfe GTFS ZIP.

Only stdlib; no SQLite or permanently running Python backend.
"""
import argparse
import csv
from datetime import date, datetime, time, timedelta
from hashlib import sha256
import io
import json
from pathlib import Path
import re
import struct
from urllib.request import Request, urlopen
import zipfile
import zlib
from zoneinfo import ZoneInfo

DEFAULT_SOURCE = 'https://ssl.renfe.com/gtransit/Fichero_CER/google_transit.zip'
TZ = ZoneInfo('Europe/Madrid')
PRIMARY_HDR = struct.Struct('<8sIIqqqI')
PRIMARY_REC = struct.Struct('<32sqIHH')
UPSTREAM_HDR = struct.Struct('<8sII')
UPSTREAM_REC = struct.Struct('<32sH14sii')
BUNDLE_HDR = struct.Struct('<8sIIIqqq32sI')  # 80 bytes
assert PRIMARY_HDR.size == 44 and PRIMARY_REC.size == 48
assert UPSTREAM_HDR.size == 16 and UPSTREAM_REC.size == 56
assert BUNDLE_HDR.size == 80


def fnv1a(buf):
    n = 2166136261
    for b in buf:
        n = ((n ^ b) * 16777619) & 0xffffffff
    return n


def sec(hhmmss):
    m = re.fullmatch(r'(\d{1,2}):(\d\d):(\d\d)', hhmmss.strip())
    if not m or int(m[2]) > 59 or int(m[3]) > 59 or int(m[1]) > 72:
        raise ValueError(f'Bad GTFS time: {hhmmss!r}')
    return int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])


def utc_madrid(d, seconds=0):
    local = datetime.combine(d, time.min).replace(tzinfo=TZ) + timedelta(seconds=seconds)
    return int(local.timestamp())


def records(z, name):
    matches = [s for s in z.namelist() if s.rsplit('/', 1)[-1] == name]
    if len(matches) != 1:
        if name == 'calendar.txt' and not matches:
            return iter(())
        raise ValueError(f'Expected one {name}, got {len(matches)}')
    def stream():
        with z.open(matches[0]) as binary:
            with io.TextIOWrapper(binary, encoding='utf-8-sig', newline='') as stream:
                yield from csv.DictReader(stream)
    return stream()


def read_zip(path, start_date, num_days):
    dates = [start_date + timedelta(days=k) for k in range(num_days)]
    end_date = dates[-1]
    with zipfile.ZipFile(path) as z:
        # Renfe uses multiple route variants; use short_name, then trip suffix.
        route_lines = {}
        for r in records(z, 'routes.txt'):
            short = (r.get('route_short_name') or '').strip().upper().replace('-', '').replace(' ', '')
            if short in ('C7', 'C10'):
                route_lines[r['route_id']] = 7 if short == 'C7' else 10
        trips = {}
        for r in records(z, 'trips.txt'):
            trip = r.get('trip_id', '').strip()
            service = r.get('service_id', '').strip()
            if not trip or len(trip.encode('ascii')) >= 32:
                continue
            line = route_lines.get(r.get('route_id', ''))
            if line is None:
                line = 7 if trip.endswith('C7') else 10 if trip.endswith('C10') else None
            if line is not None and service:
                trips[trip] = (service, line)
        if not trips:
            raise ValueError('No C7/C10 trips in GTFS')
        calendar = {}
        for r in records(z, 'calendar.txt'):
            sid = r.get('service_id', '')
            if sid:
                calendar[sid] = r
        exceptions = {}
        for r in records(z, 'calendar_dates.txt'):
            day_raw = r.get('date', '')
            if not day_raw:
                continue
            try:
                d = date.fromisoformat(f'{day_raw[:4]}-{day_raw[4:6]}-{day_raw[6:8]}')
            except ValueError:
                continue
            if start_date <= d <= end_date:
                exceptions[(r['service_id'], d)] = int(r['exception_type'])
        by_trip = {}
        for r in records(z, 'stop_times.txt'):
            trip = r.get('trip_id', '')
            if trip not in trips:
                continue
            stop_id = r.get('stop_id', '').strip()
            if not stop_id:
                continue
            arr = sec(r.get('arrival_time') or r.get('departure_time') or '')
            dep = sec(r.get('departure_time') or r.get('arrival_time') or '')
            seq = int(r['stop_sequence'])
            by_trip.setdefault(trip, []).append((seq, stop_id, arr, dep))
        primary = []
        upstream = set()
        trip_counts = 0
        active_days = set()
        weekdays = ['monday','tuesday','wednesday','thursday','friday','saturday','sunday']
        for trip, (service, line) in trips.items():
            stops = sorted(by_trip.get(trip, []))
            # The direction is established by GTFS stop order, NOT headsign.
            positions = [i for i, row in enumerate(stops) if row[1] == '10002']
            valid_targets = []
            for i in positions:
                after = stops[i+1:]
                aravaca_idx = next((j for j, v in enumerate(after) if v[1] == '10001'), -1)
                if aravaca_idx >= 0 and any(v[1] == '10000' for v in after[aravaca_idx+1:]):
                    valid_targets.append(i)
            if len(valid_targets) != 1:
                continue
            target_i = valid_targets[0]
            target_seq, _, target_arr, _ = stops[target_i]
            trip_bytes = trip.encode('ascii')
            for seq, stop_id, arr, dep in stops[:target_i+1]:
                stop_bytes = stop_id.encode('ascii')
                if len(stop_bytes) >= 14 or not (0 <= seq <= 65535):
                    raise ValueError('Stop fields cannot fit binary format')
                upstream.add((trip_bytes, seq, stop_bytes, arr-target_arr, dep-target_arr))
            for d in dates:
                override = exceptions.get((service, d))
                if override == 2:
                    continue
                if override != 1:
                    cal = calendar.get(service)
                    if cal is None or not (cal['start_date'] <= d.strftime('%Y%m%d') <= cal['end_date']):
                        continue
                    if cal[weekdays[d.weekday()]] != '1':
                        continue
                arrival_unix = utc_madrid(d, target_arr)
                primary.append((arrival_unix, trip_bytes, int(d.strftime('%Y%m%d')), target_seq, line))
                active_days.add(d)
            trip_counts += 1
        if len(primary) < 10 or len(active_days) < num_days - 1:
            raise ValueError(f'Incomplete GTFS coverage: trips={len(primary)} active_days={len(active_days)}/{num_days}')
        return sorted(set(primary)), sorted(upstream), trip_counts


def build(path, out, days, start):
    primary_rows, upstream_rows, count = read_zip(path, start, days)
    pbody = b''.join(PRIMARY_REC.pack(trip, unix, day, seq, line)
                     for unix, trip, day, seq, line in primary_rows)
    ubody = b''.join(UPSTREAM_REC.pack(trip, seq, stop, arr, dep)
                     for trip, seq, stop, arr, dep in upstream_rows)
    from_ts = utc_madrid(start)
    until_ts = utc_madrid(start + timedelta(days=days))
    generated = int(datetime.now(TZ).timestamp())
    primary = PRIMARY_HDR.pack(b'POZGTFS1', 1, len(primary_rows), generated,
                               from_ts, until_ts, fnv1a(pbody)) + pbody
    upstream = UPSTREAM_HDR.pack(b'POZUPV01', len(upstream_rows), fnv1a(ubody)) + ubody
    payload = primary + upstream
    digest = sha256(payload).digest()
    hdr0 = BUNDLE_HDR.pack(b'POZBND01', 1, len(primary), len(upstream),
                           generated, from_ts, until_ts, digest, 0)
    hdr = hdr0[:-4] + struct.pack('<I', zlib.crc32(hdr0[:-4]))
    packed = hdr + payload
    if len(packed) > 0x180000:
        raise ValueError(f'Bundle {len(packed)} exceeds 1.5MiB partition')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'latest.bin').write_bytes(packed)
    info = {'generated_at': generated, 'valid_from': from_ts, 'valid_until': until_ts,
            'primary_bytes': len(primary), 'upstream_bytes': len(upstream),
            'primary_trips': len(primary_rows), 'upstream_stops': len(upstream_rows),
            'unique_direction_trips': count, 'sha256': sha256(packed).hexdigest()}
    (out / 'latest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    return info


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--zip', type=Path)
    a.add_argument('--source', default=DEFAULT_SOURCE)
    a.add_argument('--days', type=int, default=21)
    a.add_argument('--out', type=Path, default=Path('site/gtfs'))
    a.add_argument('--start', type=date.fromisoformat, default=datetime.now(TZ).date())
    args = a.parse_args()
    if args.days < 7 or args.days > 30:
        a.error('Days must be 7..30')
    src = args.zip or Path('/tmp/renfe_cercanias_gtfs.zip')
    if args.zip is None:
        req = Request(args.source, headers={'User-Agent': 'PozueloGTFSIndex/1.0'})
        with urlopen(req, timeout=120) as response, src.open('wb') as f:
            total = 0
            while True:
                data = response.read(131072)
                if not data:
                    break
                f.write(data)
                total += len(data)
                if total > 60 * 1024 * 1024:
                    raise ValueError('Unexpectedly large GTFS archive')
    result = build(src, args.out, args.days, args.start)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
