#!/usr/bin/env python3
"""Upload a verified GTA shader pack, publishing its marker only after every part."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', required=True)
    ap.add_argument('--bundle-id', default='org.macnativegames.gta')
    ap.add_argument('--store', type=Path, default=Path(__file__).resolve().parents[1] / 'ios-seed-store')
    ap.add_argument('--logs', type=Path)
    ap.add_argument('--allow-network', action='store_true', help='Allow the already paired iPhone connection over Wi-Fi')
    a = ap.parse_args()
    store = a.store.resolve()
    lock = (store / '.push.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    index = json.loads((store / 'pack-index.json').read_text())
    fp = index['fingerprint']
    folder = index['folder']
    if not re.fullmatch(r'[0-9a-f]{64}', fp) or folder != 'shader-seed-' + fp:
        raise RuntimeError('Invalid full SHA-256 shader identity')
    marker = store / 'shader-seed.txt'
    if marker.read_text() != f'{folder}\n{fp}\n':
        raise RuntimeError('Shader marker does not match the pack index')
    if sha256(store / 'seed-manifest.json') != fp:
        raise RuntimeError('Derived manifest does not match its fingerprint')
    parts = index['parts']
    if not parts:
        raise RuntimeError('No shader pack parts')
    for n, part in enumerate(parts):
        if part['name'] != f'{folder}.pack.{n:03d}':
            raise RuntimeError('Non-contiguous shader pack parts')
        p = store / part['name']
        if p.stat().st_size != part['bytes'] or sha256(p) != part['sha256']:
            raise RuntimeError('Shader pack part failed local verification: ' + p.name)
    logdir = a.logs or Path(tempfile.mkdtemp(prefix='gta-seed-upload-'))
    logdir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    common = ['--device', a.device, '--domain-type', 'appDataContainer', '--domain-identifier', a.bundle_id]
    seq = 0

    def command(args, label, timeout=90):
        nonlocal seq
        seq += 1
        out = logdir / f'{seq:03d}-{label}.json'
        p = subprocess.run(['/usr/bin/xcrun', 'devicectl', *args, '--json-output', str(out)], env=env,
                           capture_output=True, text=True, timeout=timeout)
        text = p.stdout + p.stderr
        (logdir / f'{seq:03d}-{label}.log').write_text(text)
        return p.returncode, json.loads(out.read_text()) if out.exists() else {}, text

    def inventory():
        code, data, text = command(['device', 'info', 'files', *common, '--subdirectory', 'Documents/GameData', '--no-recurse', '--timeout', '45'], 'inventory')
        if code:
            raise RuntimeError('Could not inspect shader destination: ' + text[-500:])
        return {x['relativePath']: x['metadata']['size'] for x in data['result']['files'] if not x['resources']['isDirectory']}

    def wired():
        code, data, _ = command(['device', 'info', 'details', '--device', a.device, '--timeout', '20'], 'connection')
        connection = data.get('result', {}).get('connectionProperties', {})
        if code or connection.get('tunnelState') != 'connected':
            raise RuntimeError('The paired iPhone is not connected')
        if connection.get('transportType') != 'wired' and not a.allow_network:
            raise RuntimeError('Connect the iPhone by cable before uploading the shader pack')

    def copy(path, destination, expected_bytes):
        for attempt in range(1, 4):
            processes = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True)
            if any('devicectl device copy to' in line and a.device in line for line in processes.splitlines()):
                raise RuntimeError('Another transfer to this iPhone is active; no second copy was started')
            wired()
            code, _, text = command(['device', 'copy', 'to', *common, '--source', str(path), '--destination', destination, '--timeout', '240'], 'copy', timeout=255)
            if code == 0:
                return
            if 'POSIX error code 28' in text or 'No space left on device' in text:
                raise RuntimeError('iPhone returned ENOSPC; the shader marker was not advanced')
            if attempt == 3:
                raise RuntimeError('Could not copy ' + path.name + ': ' + text[-500:])
            time.sleep(3 * attempt)

    wired()
    remote = inventory()
    for n, part in enumerate(parts, 1):
        name = part['name']
        if remote.get(name) == part['bytes']:
            print(f'{n}/{len(parts)} already present: {name}', flush=True)
            continue
        print(f'{n}/{len(parts)} copying {part["bytes"]/1e6:.1f} MB', flush=True)
        copy(store / name, 'Documents/GameData/' + name, part['bytes'])
        remote = inventory()
        if remote.get(name) != part['bytes']:
            raise RuntimeError('Destination part size does not match: ' + name)
    remote = inventory()
    if any(remote.get(x['name']) != x['bytes'] for x in parts):
        raise RuntimeError('A shader pack part disappeared before marker publication')
    copy(marker, 'Documents/GameData/shader-seed.txt', marker.stat().st_size)
    readback = logdir / 'shader-seed-readback.txt'
    code, _, _ = command(['device', 'copy', 'from', *common, '--source', 'Documents/GameData/shader-seed.txt', '--destination', str(readback), '--timeout', '30'], 'marker-readback')
    if code or readback.read_bytes() != marker.read_bytes():
        raise RuntimeError('Shader marker readback failed')
    receipt = dict(status='complete', folder=folder, fingerprint=fp, verified_parts=len(parts),
                   transferred_pack_bytes=sum(x['bytes'] for x in parts), marker_readback_verified=True)
    (logdir / 'receipt.json').write_text(json.dumps(receipt, indent=2))
    print(f'Shader pack verified: {len(parts)} parts; marker read back.', flush=True)


if __name__ == '__main__':
    main()
