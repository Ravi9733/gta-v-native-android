#!/usr/bin/env python3
"""Validate or apply the pinned GTA iOS completion-ready ordering patch offline.

Default: read-only validation and JSON plan. --apply writes only the generated
iOS main.x0 image, its two generated digest headers, and assembly mtimes.
No build, signing, device connection, or game launch is performed here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
IMAGE_SIZE = 0x29F0000
TEXT_END = CAVE = 0x29EFE50
REWRITE_SHA = '463070253cdaa345f198146220c42a07fbdf1a8824ac74d1f3e69cdbaf02bb7b'
OLD_SHA = '997e7cdba04a68f87ecd5efab513fd6c10ab851a773c6001d22f9acce61586de'
NEW_SHA = '025db9435230c2e78946bacef267114ad01c1f57f99135c7b396f305a91addd1'
W_SHA = 'ebad646538dfcff8f18359f7f6ea23b9acab31ad83d44d19fcf02d2878f3a376'
X1_SHA = '7af1163b40b5b431d31d11b1a547be9572a2122452b696ce03b7eb51ffb50817'
MAIN_ROW = re.compile(
    rb'\{\s*"main"\s*,\s*"(?P<x0>[0-9a-f]{64})"\s*,\s*'
    rb'"(?P<w>[0-9a-f]{64})"\s*,\s*"(?P<x1>[0-9a-f]{64})"\s*\}'
)


class PatchError(RuntimeError):
    pass


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def branch(source: int, destination: int) -> bytes:
    delta = destination - source
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise PatchError('Unaligned or out-of-range ARM64 branch.')
    return struct.pack('<I', 0x14000000 | ((delta // 4) & 0x3FFFFFF))


# Reset a single byte before submit publishes the request. x8 is caller-saved;
# submit overwrites w8 at entry, and no x0..x7 argument or NZCV/LR is changed.
CHANGES = (
    (0x276D6A0, bytes.fromhex('1f2003d5'), branch(0x276D6A0, CAVE)),
    (0x276D6D8, bytes.fromhex('88098352bf6b2838'), bytes.fromhex('1f2003d51f2003d5')),
    (CAVE, bytes(12), bytes.fromhex('88098352bf6b2838') + branch(CAVE + 8, 0x276D6A4)),
)


def transform_image(data: bytes) -> tuple[bytes, str]:
    digest = sha(data)
    if len(data) != IMAGE_SIZE or digest not in (OLD_SHA, NEW_SHA):
        raise PatchError(f'Unknown iOS main.x0 image: size={len(data)}, sha256={digest}.')
    already = digest == NEW_SHA
    for offset, old, new in CHANGES:
        expected = new if already else old
        if data[offset:offset + len(expected)] != expected:
            raise PatchError(f'Unexpected bytes at main+{offset:#x}.')
    if data[CAVE + 12:IMAGE_SIZE] != bytes(IMAGE_SIZE - CAVE - 12):
        raise PatchError('The remaining RX padding is not zero.')
    if already:
        return data, 'already_patched'
    candidate = bytearray(data)
    for offset, old, new in CHANGES:
        candidate[offset:offset + len(old)] = new
    result = bytes(candidate)
    if len(result) != len(data) or sha(result) != NEW_SHA:
        raise PatchError('Candidate image digest verification failed.')
    return result, 'original'


def transform_header(data: bytes) -> tuple[bytes, str]:
    rows = list(MAIN_ROW.finditer(data))
    if len(rows) != 1:
        raise PatchError('Expected exactly one complete main digest row in ios_embed.h.')
    row = rows[0]
    old = row.group('x0').decode('ascii')
    if old not in (OLD_SHA, NEW_SHA):
        raise PatchError(f'Unknown main.x0 digest in ios_embed.h: {old}.')
    if row.group('w').decode('ascii') != W_SHA or row.group('x1').decode('ascii') != X1_SHA:
        raise PatchError('Unrecognized main writable/x1 image digests; preserve them and investigate.')
    result = data[:row.start('x0')] + NEW_SHA.encode('ascii') + data[row.end('x0'):]
    return result, old


def checked_file(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise PatchError(f'Expected an existing regular, non-symlink file: {path}.')
    return path.read_bytes()


def atomic_write(path: Path, data: bytes) -> None:
    mode = stat.S_IMODE(path.stat().st_mode)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f'.{path.name}.', delete=False) as stream:
            temporary = Path(stream.name)
            os.chmod(temporary, mode)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def self_test(image: bytes, header: bytes) -> None:
    """Exercise the real bytes in memory, including rejection and idempotence."""
    patched, _ = transform_image(image)
    assert sha(patched) == NEW_SHA
    assert transform_image(patched) == (patched, 'already_patched')
    original = bytearray(patched)
    for offset, old, new in CHANGES:
        assert original[offset:offset + len(new)] == new
        original[offset:offset + len(new)] = old
    assert sha(original) == OLD_SHA
    assert original_from_patched(patched) == bytes(original)
    assert transform_image(bytes(original))[0] == patched
    tampered = bytearray(original)
    tampered[0] ^= 1
    try:
        transform_image(bytes(tampered))
    except PatchError:
        pass
    else:
        raise AssertionError('An unknown image was accepted.')
    changed, _ = transform_header(header)
    assert transform_header(changed)[0] == changed
    for bad in (header + header, changed.replace(NEW_SHA.encode(), b'0' * 64)):
        try:
            transform_header(bad)
        except PatchError:
            pass
        else:
            raise AssertionError('An invalid header was accepted.')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=ROOT / 'GTA_build')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='Read-only validation (the default).')
    mode.add_argument('--apply', action='store_true', help='Apply the validated offline patch and dependency mtimes.')
    parser.add_argument('--self-test', action='store_true', help='Also test original/patched/rejected cases entirely in memory.')
    args = parser.parse_args()
    tree = args.tree.resolve()
    image_path = tree / 'ios-embed/main.x0.bin'
    header_paths = (tree / 'ios-embed/ios_embed.h', ROOT / 'ios/Gen/ios_embed.h')
    # build_ipa_gta.py uses copy2(gen, embed.S), so both mtimes must be fresh.
    assembly_paths = (tree / 'ios-embed/gen', ROOT / 'ios/Gen/embed.S')
    rewrite_path = tree / 'rewritten/local/main.patched.bin'
    native_path = tree / 'build/gta-native'
    layout_path = tree / 'analysis/runtime-layout.json'
    try:
        # Complete validation and plans for every file precede the first write.
        layout = json.loads(checked_file(layout_path))
        main_layout = next(item for item in layout if item['name'] == 'main')['layout']
        if main_layout['TEXT_END'] != TEXT_END or main_layout['RO_OFF'] != IMAGE_SIZE:
            raise PatchError('Unrecognized main TEXT_END/RO_OFF metadata.')
        rewrite = checked_file(rewrite_path)
        if sha(rewrite) != REWRITE_SHA:
            raise PatchError('The protected Mac rewrite differs from the pinned source.')
        protected = {rewrite_path: sha(rewrite)}
        if native_path.exists():
            protected[native_path] = sha(checked_file(native_path))
        image = checked_file(image_path)
        candidate, state = transform_image(image)
        if rewrite[:IMAGE_SIZE] != (image if state == 'original' else original_from_patched(candidate)):
            raise PatchError('The protected Mac text is not the original image for this patch.')
        originals = {image_path: image}
        planned = {image_path: candidate}
        for path in header_paths:
            originals[path] = checked_file(path)
            planned[path], _ = transform_header(originals[path])
        for path in assembly_paths:
            checked_file(path)
        if args.self_test:
            self_test(image, originals[header_paths[0]])
        changed = [path for path in planned if originals[path] != planned[path]]
        inputs = (image_path, *header_paths)
        latest_input = max(path.stat().st_mtime_ns for path in inputs)
        touches = [path for path in assembly_paths if changed or path.stat().st_mtime_ns <= latest_input]
        if args.apply:
            # Refuse concurrent replacement since preflight, before any mutation.
            for path, before in originals.items():
                if checked_file(path) != before:
                    raise PatchError(f'File changed during preflight: {path}.')
            for path in changed:
                atomic_write(path, planned[path])
            if touches:
                fresh = max(time.time_ns(), max(path.stat().st_mtime_ns for path in inputs) + 1)
                for path in touches:
                    os.utime(path, ns=(path.stat().st_atime_ns, fresh))
            for path, expected in planned.items():
                if checked_file(path) != expected:
                    raise PatchError(f'Independent readback failed: {path}.')
            if any(path.stat().st_mtime_ns <= image_path.stat().st_mtime_ns for path in assembly_paths):
                raise PatchError('Assembly dependency mtime is not newer than main.x0.bin.')
        for path, digest in protected.items():
            if sha(checked_file(path)) != digest:
                raise PatchError(f'Protected Mac file changed during the operation: {path}.')
        print(json.dumps(dict(
            status=('applied' if changed or touches else 'already_applied') if args.apply else 'validated_dry_run',
            image_state_before=state, source_image_sha256=sha(image), resulting_image_sha256=sha(candidate),
            files_planned_for_update=[str(path) for path in changed],
            assembly_mtimes_planned=[str(path) for path in touches],
            writes_performed=bool(args.apply and (changed or touches)), mac_files_unchanged=True,
            self_test_passed=bool(args.self_test), build_performed=False, device_contacted=False,
        ), indent=2))
        return 0
    except (PatchError, OSError, ValueError, KeyError, StopIteration, AssertionError) as error:
        print(f'Patch refused or verification failed: {error}', file=sys.stderr)
        return 2


def original_from_patched(patched: bytes) -> bytes:
    original = bytearray(patched)
    for offset, old, new in CHANGES:
        if original[offset:offset + len(new)] != new:
            raise PatchError('Unexpected patched bytes while reconstructing source identity.')
        original[offset:offset + len(new)] = old
    result = bytes(original)
    if sha(result) != OLD_SHA:
        raise PatchError('Could not reconstruct the pinned original image.')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
