#!/usr/bin/env python3
"""Refresh the Pozuelo GTFS bundle and publish site/gtfs/latest.*.

GitHub Actions entry point (run at repository root):
    python scripts/refresh_gtfs.py

For offline tests with a previously downloaded Renfe GTFS:
    python scripts/refresh_gtfs.py --zip /path/to/feed.zip --no-reuse

Uses only Python standard library, as does build_gtfs_bundle.py.
"""
import argparse
from datetime import date, datetime, timezone
from hashlib import sha256
import io
import json
import os
from pathlib import Path
import tempfile
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
import zipfile
from zoneinfo import ZoneInfo

from build_gtfs_bundle import DEFAULT_SOURCE, build

SITE = 'https://alvorak.github.io/renfe-pozuelo-esp32/gtfs/'
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'site' / 'gtfs'
SOURCE = os.environ.get('GTFS_SOURCE_URL', '').strip() or DEFAULT_SOURCE
BUNDLE_CAP = 0x300000  # Must match partitions.csv and gtfs_auto.c after US-07
SOURCE_CAP = 60 * 1024 * 1024


def fetch(url, timeout=120, cap=SOURCE_CAP):
    req = Request(url, headers={
        'User-Agent': 'PozueloGTFSIndex/1.1',
        'Accept': 'application/zip,application/json,application/octet-stream,*/*',
    })
    with urlopen(req, timeout=timeout) as response:
        data = response.read(cap + 1)
    if len(data) > cap:
        raise ValueError(f'Download exceeds {cap} bytes: {url}')
    if not data:
        raise ValueError(f'Empty download: {url}')
    return data


def validate_source_zip(data):
    """Reject HTML, truncated ZIPs and non-GTFS archives before publishing."""
    if not zipfile.is_zipfile(io.BytesIO(data)):
        raise ValueError('Renfe response was not a ZIP (possibly HTML/changed URL)')
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = [name.rsplit('/', 1)[-1] for name in z.namelist()]
        mandatory = {'routes.txt', 'trips.txt', 'stop_times.txt'}
        missing = mandatory - set(names)
        if missing:
            raise ValueError('ZIP is not a GTFS feed: missing ' + ', '.join(sorted(missing)))
        if not ({'calendar.txt', 'calendar_dates.txt'} & set(names)):
            raise ValueError('GTFS has neither calendar.txt nor calendar_dates.txt')
        bad = z.testzip()
        if bad is not None:
            raise ValueError(f'Corrupted GTFS member: {bad}')
    print(f'Validated GTFS ZIP ({len(data)} bytes)')


def maybe_reuse(source_digest, out=OUT):
    """Reuse only an intact, matching package with >10 days remaining."""
    try:
        info = json.loads(fetch(SITE + 'latest.json', timeout=15, cap=16000))
        remaining = int(info['valid_until']) - int(datetime.now(timezone.utc).timestamp())
        if info.get('source_sha256') != source_digest or remaining < 10 * 86400:
            print('Regenerating: source changed or package expires in <10 days')
            return False
        old_bundle = fetch(SITE + 'latest.bin', timeout=45, cap=0x300000)
        if sha256(old_bundle).hexdigest() != info['sha256']:
            raise ValueError('Published latest.bin SHA256 mismatch')
        if len(old_bundle) < 80 or old_bundle[:8] != b'POZBND01':
            raise ValueError('Published latest.bin has an invalid header')
        out.mkdir(parents=True, exist_ok=True)
        (out / 'latest.bin').write_bytes(old_bundle)
        (out / 'latest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
        print('Reused published GTFS bundle, >10 days remaining')
        return True
    except (HTTPError, URLError, TimeoutError, OSError, ValueError,
            KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f'Published GTFS not reusable: {exc}')
        return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zip', type=Path, help='Use local GTFS ZIP instead of network')
    parser.add_argument('--out', type=Path, default=OUT)
    parser.add_argument('--days', type=int, default=21)
    parser.add_argument('--start', type=date.fromisoformat)
    parser.add_argument('--no-reuse', action='store_true', help='Always regenerate latest.*')
    args = parser.parse_args(argv)
    if not 7 <= args.days <= 30:
        parser.error('--days must be 7..30')
    today_madrid = args.start or datetime.now(ZoneInfo('Europe/Madrid')).date()
    source_label = str(args.zip) if args.zip else SOURCE
    print('GTFS source:', source_label)

    if args.zip is not None:
        raw = args.zip.read_bytes()
        if len(raw) > SOURCE_CAP:
            raise ValueError('Local GTFS ZIP exceeds 60 MiB')
    else:
        try:
            raw = fetch(SOURCE)
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise RuntimeError(f'Cannot download Renfe GTFS from {SOURCE}: {exc}') from exc
    validate_source_zip(raw)
    source_digest = sha256(raw).hexdigest()
    print('Source SHA256:', source_digest)
    if not args.no_reuse and args.zip is None and maybe_reuse(source_digest, args.out):
        return

    with tempfile.TemporaryDirectory(prefix='pozuelo_gtfs_') as folder:
        path = Path(folder) / 'renfe_cercanias.zip'
        path.write_bytes(raw)
        info = build(path, args.out, args.days, today_madrid)

    if not info.get('primary_trips') or not info.get('upstream_stops'):
        raise RuntimeError('Generated GTFS indices are empty: refusing to publish')
    bundle = (args.out / 'latest.bin').read_bytes()
    if len(bundle) > BUNDLE_CAP or sha256(bundle).hexdigest() != info['sha256']:
        raise RuntimeError('Generated bundle size/SHA256 invalid: refusing to publish')
    info['source_sha256'] = source_digest
    info['source_url'] = source_label
    (args.out / 'latest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print('Created valid GTFS package:', json.dumps(info))


if __name__ == '__main__':
    main()
