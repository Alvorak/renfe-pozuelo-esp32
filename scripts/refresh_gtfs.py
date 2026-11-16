#!/usr/bin/env python3
"""Scheduled refresh: rebuild only if the GTFS source changed or coverage is low."""
from datetime import datetime, timezone
from hashlib import sha256
import json
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import urlopen, Request
import shutil
import sys

from build_gtfs_bundle import DEFAULT_SOURCE, build
from zoneinfo import ZoneInfo

SITE = 'https://alvorak.github.io/renfe-pozuelo-esp32/gtfs/'
OUT = Path('site/gtfs')
ZIP = Path('gtfs_download.zip')


def fetch(url, timeout=120, cap=60 * 1024 * 1024):
    req = Request(url, headers={'User-Agent': 'PozueloGTFSIndex/1.0'})
    with urlopen(req, timeout=timeout) as response:
        data = response.read(cap + 1)
    if len(data) > cap:
        raise ValueError('Size cap exceeded: ' + url)
    return data


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        raw = fetch(DEFAULT_SOURCE)
        print('GTFS source: Renfe official')
    except (HTTPError, URLError, TimeoutError) as exc:
        print('Renfe source not reachable:', exc)
        raw = fetch('https://gtfs-source-feeds.transit.land/es-nap-1130.zip')
        print('GTFS source: Transitland NAP mirror')
    ZIP.write_bytes(raw)
    source_hash = sha256(raw).hexdigest()
    try:
        old = json.loads(fetch(SITE + 'latest.json', timeout=15, cap=16000))
        remaining = int(old['valid_until']) - int(datetime.now(timezone.utc).timestamp())
        if old.get('source_sha256') == source_hash and remaining >= 10 * 86400:
            data = fetch(SITE + 'latest.bin', timeout=45, cap=1600000)
            if sha256(data).hexdigest() != old['sha256']:
                raise ValueError('Existing bundle SHA256 mismatch')
            (OUT / 'latest.bin').write_bytes(data)
            (OUT / 'latest.json').write_text(json.dumps(old, indent=2) + '\n', encoding='utf-8')
            print('Source unchanged and coverage sufficient; reused validated image')
            return
    except (HTTPError, URLError, ValueError, KeyError, json.JSONDecodeError, OSError) as exc:
        print('Cannot reuse old image:', exc)
    start = datetime.now(ZoneInfo('Europe/Madrid')).date()
    info = build(ZIP, OUT, 21, start)
    info['source_sha256'] = source_hash
    (OUT / 'latest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print('Created new bundle:', json.dumps(info))

if __name__ == '__main__':
    main()
