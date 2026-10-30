#!/usr/bin/env python3
"""Complete missing iPhone metallibs in a private copy of an existing GTA seed."""
import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from pack_seed_gta import PackError, IOS_TARGET, VARIANT, digest, file_digest, json_bytes, runtime_chain
from compile_seed_gta import METAL_FLAGS

FLAGS = METAL_FLAGS
SDK = 'iphoneos'
ENV = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')


def atomic_bytes(path, data):
    temporary = path.with_name(path.name + '.completion-tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def valid_library(path):
    data = path.read_bytes()
    target = IOS_TARGET.search(data)
    if not data.startswith(b'MTLB') or not target or target.group(1) or b'-apple-macos' in data:
        raise PackError(f'not an iPhone-device metallib: {path.name}')
    if b'air.compile.fast_math_disable' not in data:
        raise PackError(f'metallib does not attest safe math: {path.name}')
    return digest(data), target.group(0).decode()


def snapshot_file(source, target, expected):
    """APFS clone is independent of later source-cache writes; never hardlink."""
    if target.exists():
        if file_digest(target) == expected:
            return
        raise PackError(f'existing private snapshot differs: {target.name}')
    libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
    clone = libc.clonefile
    clone.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    clone.restype = ctypes.c_int
    if clone(os.fsencode(source), os.fsencode(target), 0):
        shutil.copyfile(source, target)
    if file_digest(target) != expected:
        target.unlink(missing_ok=True)
        raise PackError(f'SHA-256 mismatch while snapshotting {source.name}')


def command(args):
    result = subprocess.run(args, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise PackError(f"{' '.join(args[:5])}: {(result.stderr or result.stdout)[-2400:]}")
    return result.stdout


def complete(source, pack_index, output, workers):
    started = time.time()
    source, pack_index, output = source.resolve(), pack_index.resolve(), output.resolve()
    if workers < 1 or workers > 4:
        raise PackError('workers must be between 1 and 4')
    if output == source or source in output.parents:
        raise PackError('completion output must be outside the source cache')
    index_raw = pack_index.read_bytes()
    index = json.loads(index_raw)
    baseline_raw = (pack_index.parent / 'seed-manifest.json').read_bytes()
    if digest(baseline_raw) != index['manifest']['sha256']:
        raise PackError('baseline pack manifest SHA-256 differs from its pack-index')
    baseline = json.loads(baseline_raw)
    chain = runtime_chain()
    if baseline.get('schema') != 1 or baseline.get('chain') != chain:
        raise PackError('baseline manifest does not match the runtime seed contract')
    missing = sorted({row['variant'] for row in index['report']['skipped_incomplete_variants']})
    base_variants = {n.removesuffix('_b.metal') for n in baseline['files'] if n.endswith('_b.metal')}
    if not missing or set(missing) & base_variants or any(not VARIANT.fullmatch(v) for v in missing):
        raise PackError('invalid or overlapping completion variant list')
    selected = base_variants | set(missing)
    output.mkdir(parents=True, exist_ok=True)
    work = output / '.completion-work'
    work.mkdir(exist_ok=True)
    receipt_path = output / 'completion-receipt.json'
    receipt = dict(schema=1, status='snapshotting', source=str(source), baseline_fingerprint=index['fingerprint'],
                   baseline_pack_index_sha256=digest(index_raw), chain=chain, workers=workers,
                   target_variants=len(missing), baseline_variants=len(base_variants), completed=0, failed=0,
                   started_unix=started)
    def update(**values):
        receipt.update(values, updated_unix=time.time())
        atomic_bytes(receipt_path, json_bytes(receipt))
    update()
    try:
        files = {}
        # Baseline executable bytes retain their previously verified immutable hashes.
        immutable = {n: sha for n, sha in baseline['files'].items() if not n.endswith('.variants')}
        for number, (name, sha) in enumerate(sorted(immutable.items()), 1):
            snapshot_file(source / name, output / name, sha)
            files[name] = sha
            if number % 2000 == 0:
                print(f'snapshot {number}/{len(immutable)} verified immutable files', flush=True)
                update(snapshot_files=number)
        reflections = {}
        for variant in sorted(selected):
            if variant in missing:
                for end in ('_b.metal', '_b.json'):
                    name = variant + end
                    raw = (source / name).read_bytes()
                    sha = digest(raw)
                    target = output / name
                    if target.exists() and file_digest(target) != sha:
                        raise PackError(f'new source changed since private snapshot: {name}')
                    if not target.exists():
                        atomic_bytes(target, raw)
                    files[name] = sha
            reflection = json.loads((output / (variant + '_b.json')).read_bytes())
            if not isinstance(reflection, dict) or reflection.get('chain') != chain or not isinstance(reflection.get('deps'), dict):
                raise PackError(f'invalid reflection identity: {variant}')
            reflections[variant] = reflection
        found, dropped, row_count = set(), set(), 0
        for path in sorted(source.glob('*.variants')):
            if not re.fullmatch(r'[0-9a-f]{64}\.variants', path.name):
                continue
            rows = json.loads(path.read_bytes())
            if not isinstance(rows, list):
                raise PackError(f'invalid index: {path.name}')
            kept, seen = [], set()
            for row in rows:
                variant = row.get('variant') if isinstance(row, dict) else None
                if variant not in selected:
                    if isinstance(variant, str):
                        dropped.add(variant)
                    continue
                if not variant.startswith(path.stem + '-') or row.get('deps') != reflections[variant]['deps']:
                    raise PackError(f'index/reflection dependency disagreement: {variant}')
                serialized = json.dumps(row, sort_keys=True, separators=(',', ':'))
                if serialized in seen:
                    continue
                seen.add(serialized)
                kept.append(row)
                found.add(variant)
                row_count += 1
            if kept:
                raw = json_bytes(kept)
                atomic_bytes(output / path.name, raw)
                files[path.name] = digest(raw)
        if found != selected:
            raise PackError(f'{len(selected - found)} selected variants have no matching index record')
        version = command(['xcrun', '--sdk', SDK, 'metal', '--version'])
        cache = source.with_name(source.name + '.ios-metallibs')
        update(status='compiling', snapshot_files=len(files), sdk=SDK, compiler_version=version.strip(),
               metal_flags=FLAGS, selected_variants=len(selected), index_rows=row_count,
               unexpected_new_variants_excluded=sorted(dropped))
        previous = {}
        result_path = output / 'compilation-results.jsonl'
        if result_path.exists():
            for line in result_path.read_text().splitlines():
                try:
                    value = json.loads(line)
                    previous[value['variant']] = value
                except (ValueError, KeyError):
                    pass
        def compile_one(variant):
            name = variant + '_b'
            msl = output / (name + '.metal')
            raw = msl.read_bytes()
            if digest(raw) != files[msl.name]:
                raise PackError(f'private MSL changed before compilation: {variant}')
            key = digest(raw + '\0'.join([SDK, version, *FLAGS]).encode())
            library = output / (name + '.metallib')
            old = previous.get(variant, {})
            if library.exists() and old.get('compile_key') == key:
                sha, target = valid_library(library)
                if sha == old.get('sha256'):
                    return dict(variant=variant, name=library.name, sha256=sha, target=target,
                                compile_key=key, source_sha256=digest(raw), reused='private_receipt')
            cached = cache / (key + '.metallib')
            if cached.is_file():
                try:
                    sha, target = valid_library(cached)
                    if library.exists():
                        library.unlink()
                    snapshot_file(cached, library, sha)
                    return dict(variant=variant, name=library.name, sha256=sha, target=target,
                                compile_key=key, source_sha256=digest(raw), reused='compiler_cache')
                except PackError:
                    pass
            air, built = work / (name + '.air'), work / (name + '.metallib')
            command(['xcrun', '--sdk', SDK, 'metal', *FLAGS, '-c', str(msl), '-o', str(air)])
            try:
                command(['xcrun', '--sdk', SDK, 'metallib', str(air), '-o', str(built)])
                sha, target = valid_library(built)
                os.replace(built, library)
                return dict(variant=variant, name=library.name, sha256=sha, target=target,
                            compile_key=key, source_sha256=digest(raw), reused=False)
            finally:
                air.unlink(missing_ok=True)
        completed, failures, reused = 0, [], 0
        with result_path.open('a') as results, concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(compile_one, v): v for v in missing}
            for future in concurrent.futures.as_completed(futures):
                variant = futures[future]
                try:
                    result = future.result()
                    files[result['name']] = result['sha256']
                    completed += 1
                    reused += bool(result['reused'])
                    result.update(status='complete', finished_unix=time.time())
                    results.write(json.dumps(result, sort_keys=True) + '\n')
                except Exception as error:
                    failures.append(dict(variant=variant, error=str(error)))
                    results.write(json.dumps(dict(variant=variant, status='failed', error=str(error))) + '\n')
                results.flush()
                if (completed + len(failures)) % 25 == 0 or completed + len(failures) == len(missing):
                    print(f'iOS metallibs {completed}/{len(missing)}, failures={len(failures)}, reused={reused}', flush=True)
                    update(completed=completed, failed=len(failures), reused=reused)
        if failures:
            raise PackError(f'{len(failures)} metallib compilations failed; first: {failures[0]}')
        update(status='verifying', completed=completed, failed=0, reused=reused)
        for number, (name, sha) in enumerate(sorted(files.items()), 1):
            if file_digest(output / name) != sha:
                raise PackError(f'final snapshot SHA-256 mismatch: {name}')
            if name.endswith('_b.metallib'):
                valid_library(output / name)
            if number % 4000 == 0:
                print(f'final verify {number}/{len(files)}', flush=True)
        manifest = dict(schema=1, chain=chain, shaders=len(selected), ios_metallibs=len(selected),
                        ios_metal_flags=' '.join(FLAGS), source_manifest_sha256=digest(baseline_raw),
                        files=dict(sorted(files.items())))
        raw = json_bytes(manifest)
        atomic_bytes(output / 'seed-manifest.json', raw)
        update(status='complete', completed=completed, failed=0, reused=reused,
               final_variants=len(selected), final_files=len(files), manifest_sha256=digest(raw),
               elapsed_seconds=round(time.time() - started, 3))
        print(f'COMPLETE: {len(selected)} variants; input seed at {output}', flush=True)
        return receipt
    except BaseException as error:
        update(status='failed', error=str(error), elapsed_seconds=round(time.time() - started, 3))
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--pack-index', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=4)
    args = parser.parse_args()
    try:
        complete(args.source, args.pack_index, args.out, args.workers)
    except Exception as error:
        print(f'completion failed: {error}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
