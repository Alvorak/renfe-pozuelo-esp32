#!/usr/bin/env python3
"""Build and publish the compact Pozuelo GTFS bundle from Renfe official GTFS.

The published Renfe Cercanias source is defined by DEFAULT_SOURCE in
build_gtfs_bundle.py.  The old unauthenticated Transitland mirror is not used:
it returns HTTP 401 and hides the real failure.

Optional GTFS_SOURCE_URL environment variable permits future source migrations.
No change to the ESP32 firmware is needed for this script.
"""

from datetime import datetime, timezone
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


def fetch(url, timeout=120, cap=60 * 1024 * 1024):
    req = Request(url, headers={'User-Agent': 'PozueloGTFSIndex/1.1',
                                'Accept': 'application/zip,application/json,application/octet-stream,*/*'})
    with urlopen(req, timeout=timeout) as response:
        data = response.read(cap + 1)
    if len(data) > cap:
        raise ValueError('Download larger than allowed cap: ' + url)
    if not data:
        raise ValueError('Empty download: ' + url)
    return data


def validate_source_zip(data):
    """Fail closed on a wrong resource, HTML login page, or truncated GTFS."""
    if not zipfile.is_zipfile(io.BytesIO(data)):
        raise ValueError('Renfe response was not a ZIP (maybe HTML / changed URL)')
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = [name.rsplit('/', 1)[-1] for name in z.namelist()]
        mandatory = {'routes.txt', 'trips.txt', 'stop_times.txt'}
        missing = mandatory - set(names)
        if missing:
            raise ValueError('ZIP is not a GTFS feed: missing ' + ', '.join(sorted(missing)))
        if 'calendar.txt' not in names and 'calendar_dates.txt' not in names:
            raise ValueError('GTFS has neither calendar.txt nor calendar_dates.txt')
    print(f'Validated source ZIP: {len(data)} bytes, GTFS files present')


def maybe_reuse(source_digest):
    try:
        info = json.loads(fetch(SITE + 'latest.json', timeout=15, cap=16000))
        remaining = int(info['valid_until']) - int(datetime.now(timezone.utc).timestamp())
        if info.get('source_sha256') != source_digest or remaining < 10 * 86400:
            print('Generating new package: source changed or coverage below 10 days')
            return False
        old_bundle = fetch(SITE + 'latest.bin', timeout=45, cap=1600000)
        if sha256(old_bundle).hexdigest() != info['sha256']:
            raise ValueError('Published latest.bin has incorrect SHA256')
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'latest.bin').write_bytes(old_bundle)
        (OUT / 'latest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
        print('Source unchanged; reused published bundle with >10 days remaining')
        return True
    except (HTTPError, URLError, TimeoutError, OSError, ValueError, KeyError,
            TypeError, json.JSONDecodeError) as exc:
        print(f'No reusable published bundle: {exc}')
        return False


def main():
    print('GTFS Cercanias source:', SOURCE)
    try:
        raw = fetch(SOURCE)
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise RuntimeError(
            f'Cannot download Renfe GTFS from {SOURCE}: {exc}. '
            'Check the official catalog; do not use an unauthenticated Transitland URL.'
        ) from exc
    validate_source_zip(raw)
    source_digest = sha256(raw).hexdigest()
    print('Source SHA256:', source_digest)

    if maybe_reuse(source_digest):
        return

    today_madrid = datetime.now(ZoneInfo('Europe/Madrid')).date()
    with tempfile.TemporaryDirectory(prefix='pozuelo_gtfs_') as folder:
        source_path = Path(folder) / 'renfe_cercanias.zip'
        source_path.write_bytes(raw)
        OUT.mkdir(parents=True, exist_ok=True)
        info = build(source_path, OUT, 21, today_madrid)

    if not info.get('primary_trips') or not info.get('upstream_stops'):
        raise RuntimeError('Generated GTFS indices are empty; refusing to publish')
    bin_path = OUT / 'latest.bin'
    if sha256(bin_path.read_bytes()).hexdigest() != info['sha256']:
        raise RuntimeError('Generated bundle SHA256 mismatch; refusing to publish')

    info['source_sha256'] = source_digest
    info['source_url'] = SOURCE
    (OUT / 'latest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print('Created valid GTFS package:', json.dumps(info))


if __name__ == '__main__':
    main()
