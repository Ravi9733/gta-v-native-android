#!/usr/bin/env python3
"""Pin and compose two GTA iOS-only ordering fixes, entirely offline.

Default/check/self-test are read-only. --apply may update only generated iOS
main.x0.bin, the two digest headers, and assembly dependency mtimes. This wrapper
supersedes the build's async-ready invocation; that strict script stays intact.
No compilation, signing, device access, or game execution is performed here.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
ASYNC_SOURCE_SHA = '8ffa0cc6719668a89b19f2faa0ccfc7fe73683c635e55529c9b9d966df71eb9c'
BASE_SHA = '997e7cdba04a68f87ecd5efab513fd6c10ab851a773c6001d22f9acce61586de'
READY_SHA = '025db9435230c2e78946bacef267114ad01c1f57f99135c7b396f305a91addd1'
COMPOSED_SHA = '8f229188029e43a828c839daaab3160019efeb1d32d1c436cf5fd5a260e88ad2'
IMAGE_SIZE = 0x29F0000
SITE, RETURN, HELPER, CAVE = 0x4028, 0x402C, 0x1E780, 0x29EFE60
PAYLOAD_HEX = 'c80e148b092d40f9e827bfa9132d00f944ba5897e827c1a840000035092d00f9beb0feb0deb3009169505817'
CONTEXT_PINS = (
    (0x3CB0, 44, 'dc47d326b9e2f9dbedeaff13f63c6b860c9445edf9c87c205ec85083939eb542'),
    (0x4028, 44, 'c8b774b0b0630007d56e9316a597e5b07de629c6d09352e574d4f57a4931244e'),
    (0x1E780, 328, '01b065a98b95924f7afae19653f4614ca177269980ffffd366d4e649b7afd22a'),
    (0x4120, 120, 'be3b143960cf8d18fffddb7a88a6768052a7fce41727f8dd135493d29c6d101b'),
)


class PatchError(RuntimeError):
    pass


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checked_file(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise PatchError(f'Expected a regular non-symlink file: {path}')
    return path.read_bytes()


def branch(source: int, destination: int, link: bool = False) -> bytes:
    delta = destination - source
    if source % 4 or destination % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise PatchError('Unaligned or out-of-range ARM64 B/BL.')
    return struct.pack('<I', (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x3FFFFFF))


def adrp(source: int, destination: int, register: int) -> bytes:
    pages = (destination >> 12) - (source >> 12)
    if not 0 <= register <= 30 or not -(1 << 20) <= pages < (1 << 20):
        raise PatchError('Out-of-range ARM64 ADRP.')
    return struct.pack('<I', 0x90000000 | ((pages & 3) << 29) |
                       (((pages >> 2) & 0x7FFFF) << 5) | register)


def payload() -> bytes:
    # x8 is overwritten at the original helper entry; x9 is caller-saved.
    # Save slot address + exact prior value across the original helper on a
    # 16-byte-aligned temporary stack area. No argument/callee-saved register or
    # NZCV is changed. Failure restores the prior slot, then original CBZ and
    # cleanup at 0x4120..0x4194 run unchanged. Restore the LR the original BL set.
    result = b''.join((
        bytes.fromhex('c80e148b'),       # add x8,x22,x20,lsl #3
        bytes.fromhex('092d40f9'),       # ldr x9,[x8,#0x58]
        bytes.fromhex('e827bfa9'),       # stp x8,x9,[sp,#-16]!
        bytes.fromhex('132d00f9'),       # str x19,[x8,#0x58]
        branch(CAVE + 16, HELPER, True),
        bytes.fromhex('e827c1a8'),       # ldp x8,x9,[sp],#16
        bytes.fromhex('40000035'),       # cbnz w0,CAVE+32 (does not change NZCV)
        bytes.fromhex('092d00f9'),       # str x9,[x8,#0x58] (failure only)
        adrp(CAVE + 32, RETURN, 30),
        struct.pack('<I', 0x91000000 | ((RETURN & 0xFFF) << 10) | (30 << 5) | 30),
        branch(CAVE + 40, RETURN),
    ))
    if result.hex() != PAYLOAD_HEX:
        raise PatchError('Machine-patch encoding differs from the reviewed payload.')
    return result


def async_module():
    path = ROOT / 'ios/patch_gta_async_ready.py'
    data = checked_file(path)
    if sha(data) != ASYNC_SOURCE_SHA:
        raise PatchError('The pinned async-ready verifier changed; review composition first.')
    spec = importlib.util.spec_from_file_location('gta_pinned_async_ready', path)
    if spec is None or spec.loader is None:
        raise PatchError('Cannot load the pinned async-ready verifier.')
    module = importlib.util.module_from_spec(spec)
    # Avoid a __pycache__ write during a read-only check.
    prior = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = prior
    if (module.OLD_SHA, module.NEW_SHA, module.IMAGE_SIZE) != (BASE_SHA, READY_SHA, IMAGE_SIZE):
        raise PatchError('Async-ready identity constants differ.')
    return module


def remove_worker(data: bytes) -> bytes:
    if len(data) != IMAGE_SIZE or sha(data) != COMPOSED_SHA:
        raise PatchError('Only the exact composed image can be normalized.')
    code = payload()
    if data[SITE:SITE + 4] != branch(SITE, CAVE) or data[CAVE:CAVE + len(code)] != code:
        raise PatchError('Unexpected worker-publish patch bytes.')
    original = bytearray(data)
    original[SITE:SITE + 4] = branch(SITE, HELPER, True)
    original[CAVE:CAVE + len(code)] = bytes(len(code))
    if sha(original) != READY_SHA:
        raise PatchError('Worker normalization did not recover the pinned ready image.')
    return bytes(original)


def transform_image(data: bytes, module=None) -> tuple[bytes, str]:
    module = module or async_module()
    digest = sha(data)
    states = {BASE_SHA: 'base', READY_SHA: 'async_ready', COMPOSED_SHA: 'already_composed'}
    if len(data) != IMAGE_SIZE or digest not in states:
        raise PatchError(f'Unknown iOS main.x0 image: size={len(data)}, sha256={digest}')
    normalized = remove_worker(data) if digest == COMPOSED_SHA else data
    ready, _ = module.transform_image(normalized)
    if sha(ready) != READY_SHA:
        raise PatchError('The async-ready verifier did not produce the pinned ready image.')
    for offset, size, expected in CONTEXT_PINS:
        if sha(ready[offset:offset + size]) != expected:
            raise PatchError(f'Unrecognized worker/caller/failure context at main+{offset:#x}.')
    code = payload()
    if ready[SITE:SITE + 4] != branch(SITE, HELPER, True):
        raise PatchError('Original worker-start BL differs.')
    if ready[CAVE:IMAGE_SIZE] != bytes(IMAGE_SIZE - CAVE):
        raise PatchError('The worker RX cave is not empty.')
    result = bytearray(ready)
    result[SITE:SITE + 4] = branch(SITE, CAVE)
    result[CAVE:CAVE + len(code)] = code
    if sha(result) != COMPOSED_SHA:
        raise PatchError('Composed image digest differs from the reviewed result.')
    return bytes(result), states[digest]


def transform_header(data: bytes, module=None) -> tuple[bytes, str]:
    module = module or async_module()
    rows = list(module.MAIN_ROW.finditer(data))
    if len(rows) != 1:
        raise PatchError('Expected exactly one complete main digest row.')
    row = rows[0]
    old = row.group('x0').decode('ascii')
    if old == COMPOSED_SHA:
        normalized = data[:row.start('x0')] + READY_SHA.encode() + data[row.end('x0'):]
    else:
        normalized = data
    ready, _ = module.transform_header(normalized)
    ready_row = list(module.MAIN_ROW.finditer(ready))[0]
    result = ready[:ready_row.start('x0')] + COMPOSED_SHA.encode() + ready[ready_row.end('x0'):]
    return result, old


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


def self_test(image: bytes, header: bytes, module) -> dict:
    composed, _ = transform_image(image, module)
    ready = remove_worker(composed)
    base = module.original_from_patched(ready)
    for source in (base, ready, composed):
        assert transform_image(source, module)[0] == composed
    changed, _ = transform_header(header, module)
    assert transform_header(changed, module)[0] == changed
    negative = []
    tampered = bytearray(composed)
    tampered[CAVE + 28] ^= 1  # corrupt failure rollback STR
    unknown = bytearray(ready)
    unknown[0] ^= 1
    for name, operation in (
        ('unknown_image', lambda: transform_image(bytes(unknown), module)),
        ('corrupt_failure_rollback', lambda: transform_image(bytes(tampered), module)),
        ('duplicate_header', lambda: transform_header(header + header, module)),
        ('changed_writable_digest', lambda: transform_header(header.replace(module.W_SHA.encode(), b'0' * 64), module)),
        ('out_of_range_branch', lambda: branch(SITE, SITE + (1 << 27))),
    ):
        try:
            operation()
        except (PatchError, module.PatchError):
            negative.append(name)
        else:
            raise AssertionError(f'Negative control accepted: {name}')
    return {'states_composed': 3, 'header_idempotent': True, 'negative_controls_rejected': negative}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=ROOT / 'GTA_build')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true')
    mode.add_argument('--apply', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    try:
        module = async_module()
        tree = args.tree.resolve()
        image_path = tree / 'ios-embed/main.x0.bin'
        header_paths = (tree / 'ios-embed/ios_embed.h', ROOT / 'ios/Gen/ios_embed.h')
        assembly_paths = (tree / 'ios-embed/gen', ROOT / 'ios/Gen/embed.S')
        rewrite_path = tree / 'rewritten/local/main.patched.bin'
        native_path = tree / 'build/gta-native'
        layout = json.loads(checked_file(tree / 'analysis/runtime-layout.json'))
        main_layout = next(item for item in layout if item['name'] == 'main')['layout']
        if main_layout['TEXT_END'] != module.TEXT_END or main_layout['RO_OFF'] != IMAGE_SIZE:
            raise PatchError('Unrecognized main layout metadata.')
        rewrite = checked_file(rewrite_path)
        if sha(rewrite) != module.REWRITE_SHA:
            raise PatchError('The protected Mac rewrite differs from the pinned source.')
        protected = {rewrite_path: sha(rewrite), ROOT / 'ios/patch_gta_async_ready.py': ASYNC_SOURCE_SHA}
        if native_path.exists():
            protected[native_path] = sha(checked_file(native_path))
        image = checked_file(image_path)
        candidate, state = transform_image(image, module)
        ready = remove_worker(candidate)
        if rewrite[:IMAGE_SIZE] != module.original_from_patched(ready):
            raise PatchError('The protected Mac rewrite is not the pinned unmodified source.')
        originals, planned = {image_path: image}, {image_path: candidate}
        for path in header_paths:
            originals[path] = checked_file(path)
            planned[path], _ = transform_header(originals[path], module)
        for path in assembly_paths:
            checked_file(path)
        tests = self_test(image, originals[header_paths[0]], module) if args.self_test else None
        changed = [path for path in planned if originals[path] != planned[path]]
        latest = max(path.stat().st_mtime_ns for path in originals)
        touches = [path for path in assembly_paths if changed or path.stat().st_mtime_ns <= latest]
        if args.apply:
            for path, before in originals.items():
                if checked_file(path) != before:
                    raise PatchError(f'File changed since preflight: {path}')
            for path, expected in protected.items():
                if sha(checked_file(path)) != expected:
                    raise PatchError(f'Protected source changed since preflight: {path}')
            for path in changed:
                atomic_write(path, planned[path])
            if touches:
                fresh = max(time.time_ns(), max(path.stat().st_mtime_ns for path in originals) + 1)
                for path in touches:
                    os.utime(path, ns=(path.stat().st_atime_ns, fresh))
            for path, expected in planned.items():
                if checked_file(path) != expected:
                    raise PatchError(f'Independent readback failed: {path}')
        for path, expected in protected.items():
            if sha(checked_file(path)) != expected:
                raise PatchError(f'Protected Mac/async file changed: {path}')
        print(json.dumps(dict(status='applied' if args.apply else 'validated_dry_run',
            image_state_before=state, source_image_sha256=sha(image), resulting_image_sha256=sha(candidate),
            site=hex(SITE), original_site_bytes=branch(SITE, HELPER, True).hex(),
            patched_site_bytes=branch(SITE, CAVE).hex(), cave=hex(CAVE), payload=payload().hex(),
            files_planned_for_update=[str(p) for p in changed], assembly_mtimes_planned=[str(p) for p in touches],
            writes_performed=bool(args.apply and (changed or touches)), self_test=tests,
            mac_and_async_files_unchanged=True, build_performed=False, device_contacted=False), indent=2))
        return 0
    except Exception as error:
        print(json.dumps({'status': 'error', 'error': str(error), 'device_contacted': False}), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
