#!/usr/bin/env python3
"""Build a verified iPhone shader seed without changing its source cache."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import sys
import tempfile

PART_SIZE = 64 << 20
CHAIN_HEADER = Path(__file__).resolve().parents[1] / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
SAFE_NAME = re.compile(r'(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)\Z')
VARIANT = re.compile(r'[0-9a-f]{64}-[0-9a-f]{12}\Z')
SHA256 = re.compile(r'[0-9a-f]{64}\Z')
IOS_TARGET = re.compile(rb'air64(?:_v[0-9]+)?-apple-ios[0-9]+(?:\.[0-9]+){0,2}(-simulator)?')

class PackError(ValueError):
    pass

def digest(data):
    return hashlib.sha256(data).hexdigest()

def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()

def file_digest(path):
    sha = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(1 << 20):
            sha.update(block)
    return sha.hexdigest()

def runtime_chain():
    match = re.search(r'^\s*#define\s+DMSH_CHAIN\s+"([^"\n]+)"', CHAIN_HEADER.read_text(), re.M)
    if not match:
        raise PackError(f'DMSH_CHAIN not found in {CHAIN_HEADER}')
    return match.group(1)

def metal_flags(value):
    """Keep the compiler attestation exact; a prefix is not a compiler flag."""
    try:
        flags = shlex.split(value) if isinstance(value, str) else []
    except ValueError as error:
        raise PackError('invalid iOS Metal flags') from error
    if (not flags or flags.count('-fmetal-math-mode=safe') != 1 or
            len(flags) != len(set(flags)) or
            set(flags) - {'-fmetal-math-mode=safe', '-fpreserve-invariance'}):
        raise PackError('source manifest does not attest supported iOS safe-math compiler flags')
    return ' '.join(flags)

def prepare(seed):
    """Normalize verified runtime files and report every incomplete variant dropped."""
    raw = (seed / 'seed-manifest.json').read_bytes()
    original = json.loads(raw)
    chain = runtime_chain()
    if original.get('chain') != chain:
        raise PackError(f"source chain {original.get('chain')!r} does not match runtime {chain!r}")
    recorded = original.get('files')
    if not isinstance(recorded, dict) or not recorded:
        raise PackError('source manifest has no file hashes')
    flags = metal_flags(original.get('ios_metal_flags'))
    allowed = {n: sha for n, sha in recorded.items() if isinstance(n, str) and SAFE_NAME.fullmatch(n)}
    index_snapshots, updated_indexes = {}, []
    for name, sha in sorted(allowed.items()):
        if not isinstance(sha, str) or not SHA256.fullmatch(sha):
            raise PackError(f'invalid SHA-256 in source manifest: {name}')
        if not (seed / name).is_file():
            raise PackError(f'source manifest file is missing: {name}')
        if name.endswith('.variants'):
            # These indexes are rewritten by the Mac runtime. Trust their rows only
            # after checking them against immutable, hash-verified reflection below.
            snapshot = (seed / name).read_bytes()
            current = digest(snapshot)
            index_snapshots[name] = snapshot
            if current != sha:
                updated_indexes.append(dict(name=name, old_sha256=sha, new_sha256=current))
        elif file_digest(seed / name) != sha:
            raise PackError(f'SHA-256 mismatch: {name}')
    payloads, selected, skipped = {}, set(), {}
    reflections = {}

    def reflection_for(variant):
        if variant not in reflections:
            name = variant + '_b.json'
            data = (seed / name).read_bytes()
            if digest(data) != allowed[name]:
                raise PackError(f'SHA-256 mismatch: {name}')
            value = json.loads(data)
            if not isinstance(value, dict) or value.get('chain') != chain:
                raise PackError(f'reflection chain does not match runtime: {variant}')
            reflections[variant] = value
        return reflections[variant]

    original_rows = kept_rows = duplicate_rows = 0
    for name in sorted(n for n in allowed if n.endswith('.variants')):
        rows = json.loads(index_snapshots[name])
        if not isinstance(rows, list):
            raise PackError(f'variant index is not an array: {name}')
        kept, seen = [], set()
        for row in rows:
            original_rows += 1
            variant = row.get('variant') if isinstance(row, dict) else None
            if not isinstance(variant, str) or not VARIANT.fullmatch(variant) or not isinstance(row.get('deps'), dict):
                raise PackError(f'invalid variant record in {name}')
            if not variant.startswith(name.removesuffix('.variants') + '-'):
                raise PackError(f'variant belongs to another shader: {variant} in {name}')
            missing = [variant + end for end in ('_b.metal', '_b.json', '_b.metallib') if variant + end not in allowed]
            if missing:
                skipped[variant] = missing
                continue
            if row['deps'] != reflection_for(variant).get('deps', {}):
                raise PackError(f'variant index dependencies disagree with verified reflection: {variant} in {name}')
            identity = json.dumps(row, sort_keys=True, separators=(',', ':'))
            if identity in seen:
                duplicate_rows += 1
                continue
            seen.add(identity)
            kept.append(row)
            selected.add(variant)
            kept_rows += 1
        if kept:
            payloads[name] = json_bytes(kept)
    if not selected:
        raise PackError(f'no complete iOS shader variants; {len(skipped)} incomplete variants rejected')
    for variant in sorted(selected):
        reflection_for(variant)
        lib = (seed / (variant + '_b.metallib')).read_bytes()
        target = IOS_TARGET.search(lib)
        if not lib.startswith(b'MTLB') or not target or target.group(1) or b'-apple-macos' in lib:
            raise PackError(f'not an iPhone-device metallib: {variant}')
        for end in ('_b.metal', '_b.json', '_b.metallib'):
            payloads[variant + end] = seed / (variant + end)
    if 'pso_manifest.tsv' in allowed:
        payloads['pso_manifest.tsv'] = seed / 'pso_manifest.tsv'
    manifest = dict(schema=1, chain=chain, shaders=len(selected), ios_metallibs=len(selected),
                    ios_metal_flags=flags, source_manifest_sha256=digest(raw),
                    files={n: digest(v) if isinstance(v, bytes) else allowed[n] for n, v in sorted(payloads.items())})
    for updated in updated_indexes:
        updated['normalized_sha256'] = manifest['files'].get(updated['name'])
    report = dict(source_files=len(recorded), ignored_nonruntime_files=len(recorded) - len(allowed),
                  original_index_rows=original_rows, kept_index_rows=kept_rows,
                  duplicate_index_rows_removed=duplicate_rows,
                  skipped_incomplete_variants=[dict(variant=n, missing=skipped[n]) for n in sorted(skipped)],
                  updated_dynamic_indexes=updated_indexes)
    return manifest, payloads, report

class PartWriter:
    def __init__(self, directory, folder, size):
        if size <= 0:
            raise PackError('part size must be positive')
        self.directory, self.folder, self.limit = directory, folder, size
        self.output, self.parts, self.size = None, [], 0
    def finish_part(self):
        if self.output is not None:
            self.output.flush()
            os.fsync(self.output.fileno())
            self.output.close()
            self.parts.append(dict(name=self.path.name, bytes=self.size, sha256=self.sha.hexdigest()))
            self.output = None
    def write(self, data):
        view = memoryview(data)
        while view:
            if self.output is None:
                self.path = self.directory / f'{self.folder}.pack.{len(self.parts):03d}'
                self.output = self.path.open('xb')
                self.size, self.sha = 0, hashlib.sha256()
            count = min(len(view), self.limit - self.size)
            piece = view[:count]
            self.output.write(piece)
            self.sha.update(piece)
            self.size += count
            view = view[count:]
            if self.size == self.limit:
                self.finish_part()
    def close(self):
        self.finish_part()

def publish(stage, output, names, part_names):
    """Roll back owned outputs on failure; publish the completion marker last."""
    backup = stage / 'backup'
    backup.mkdir()
    moved = []
    try:
        for name in names:
            target = output / name
            if name in part_names and target.exists():
                if file_digest(target) != file_digest(stage / name):
                    raise PackError(f'existing pack part differs; refusing to replace it: {name}')
                continue
            previous = backup / name
            had_previous = target.exists()
            if had_previous:
                os.replace(target, previous)
            moved.append((target, previous, had_previous))
            os.replace(stage / name, target)
    except BaseException:
        for target, previous, had_previous in reversed(moved):
            if had_previous:
                os.replace(previous, target)
            else:
                target.unlink(missing_ok=True)
        raise

def build_pack(seed, output, part_size=PART_SIZE):
    seed, output = Path(seed).resolve(), Path(output).resolve()
    if output == seed or seed in output.parents:
        raise PackError('output must be outside the source cache')
    manifest, payloads, report = prepare(seed)
    manifest_data = json_bytes(manifest)
    fingerprint = digest(manifest_data)
    folder = 'shader-seed-' + fingerprint
    payloads['seed-manifest.json'] = manifest_data
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.gta-seed-pack-', dir=output) as temp:
        stage = Path(temp)
        writer = PartWriter(stage, folder, part_size)
        try:
            writer.write(b'TOTKPACK1')
            for name, value in sorted(payloads.items()):
                encoded = name.encode()
                size = len(value) if isinstance(value, bytes) else value.stat().st_size
                writer.write(struct.pack('<H', len(encoded)) + encoded + struct.pack('<Q', size))
                sha = hashlib.sha256()
                if isinstance(value, bytes):
                    writer.write(value)
                    sha.update(value)
                else:
                    with value.open('rb') as source:
                        while block := source.read(1 << 20):
                            writer.write(block)
                            sha.update(block)
                expected = fingerprint if name == 'seed-manifest.json' else manifest['files'][name]
                if sha.hexdigest() != expected:
                    raise PackError(f'source changed while packing: {name}')
            writer.write(b'\0\0')
        finally:
            writer.close()
        index = dict(schema=1, folder=folder, fingerprint=fingerprint,
                     manifest=dict(name='seed-manifest.json', bytes=len(manifest_data), sha256=fingerprint),
                     parts=writer.parts, files=len(payloads), shaders=manifest['shaders'], report=report)
        (stage / 'seed-manifest.json').write_bytes(manifest_data)
        (stage / 'pack-index.json').write_bytes(json_bytes(index))
        (stage / 'fingerprint.txt').write_text(fingerprint + '\n')
        (stage / 'shader-seed.txt').write_text(folder + '\n' + fingerprint + '\n')
        names = [p['name'] for p in writer.parts]
        publish(stage, output, names + ['seed-manifest.json', 'pack-index.json', 'fingerprint.txt', 'shader-seed.txt'], set(names))
    return index

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('seed', type=Path)
    parser.add_argument('out_dir', type=Path)
    args = parser.parse_args()
    try:
        index = build_pack(args.seed, args.out_dir)
    except (OSError, ValueError, TypeError) as error:
        print(f'shader seed packing failed: {error}', file=sys.stderr)
        return 1
    skipped = len(index['report']['skipped_incomplete_variants'])
    if skipped:
        print(f'WARNING: skipped {skipped} incomplete shader variants; details in pack-index.json', file=sys.stderr)
    refreshed = len(index['report']['updated_dynamic_indexes'])
    if refreshed:
        print(f'Refreshed {refreshed} dynamic variant indexes against verified reflection; details in pack-index.json')
    print(f"packed {index['shaders']} complete iOS shader variants, {index['files']} files into {len(index['parts'])} parts ({index['folder']})")
    return 0

if __name__ == '__main__':
    sys.exit(main())
