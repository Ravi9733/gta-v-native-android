#!/usr/bin/env python3
"""Prepare a verified GTA iPhone shader supplement locally. Never contacts a device.

Retranslates copied requests with the project's pinned translator and restores only
variants absent from a baseline manifest. Every device library is newly compiled
with compile_seed_gta.METAL_FLAGS; old libraries are never copied. The runtime
manifest is published only after every requested item and payload is verified.
"""
import argparse
import collections
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from compile_seed_gta import METAL_FLAGS
from pack_seed_gta import (IOS_TARGET, VARIANT, PackError, file_digest, json_bytes,
                           prepare, runtime_chain)

PROJECT = Path(__file__).resolve().parents[1]
SUFFIXES = ('_b.metal', '_b.json', '_b.metallib')
MAX_CODE = 16 << 20

def deps_match(deps, answers):
    # Exact port of this project's totk_shader_rt.m, not upstream's newer matcher.
    for kind, default in {'tex': '2d', 'img': {}, 'attr': 'float', 'fout': 'float'}.items():
        for slot, want in deps.get(kind, {}).items():
            have = answers.get(kind, {}).get(slot, default)
            if kind == 'tex' and want in ('*', '*_depth'):
                if not isinstance(have, str) or have.endswith('_depth') != (want == '*_depth'):
                    return False
            elif have != want:
                return False
    return True

def load_requests(folder):
    requests, hashes = {}, {}
    for path in sorted(folder.glob('*.json')):
        raw = path.read_bytes()
        rq = json.loads(raw)
        variant, code = rq.get('variant'), rq.get('code_hex')
        if (not isinstance(variant, str) or not VARIANT.fullmatch(variant) or path.stem != variant or
                rq.get('stage') not in ('vs', 'ps', 'cs') or not isinstance(code, str) or
                not re.fullmatch(r'(?:[0-9a-f]{2})+', code) or len(code) // 2 > MAX_CODE):
            raise PackError(f'invalid request: {path.name}')
        if any(not isinstance(rq.get(kind, {}), dict) for kind in ('tex', 'img', 'attr', 'fout')):
            raise PackError(f'invalid request dependencies: {path.name}')
        requests[variant], hashes[path.name] = rq, hashlib.sha256(raw).hexdigest()
    return requests, hashes

def variants(manifest):
    return {name.removesuffix('_b.metallib') for name in manifest['files']
            if name.endswith('_b.metallib') and VARIANT.fullmatch(name.removesuffix('_b.metallib'))}

def command(args, env):
    result = subprocess.run(args, env=env, capture_output=True, text=True)
    if result.returncode:
        raise PackError(f'{args[0]} failed ({result.returncode}): {result.stderr[-3000:]}')
    return result.stdout

def library(path):
    raw = path.read_bytes()
    target = IOS_TARGET.search(raw)
    if (not raw.startswith(b'MTLB') or not target or target.group(1) or b'-apple-macos' in raw or
            b'air.compile.fast_math_disable' not in raw):
        raise PackError(f'not an attested safe-math iPhone device library: {path.name}')
    return target.group().decode()

def build(args):
    started = time.time()
    root, output = args.out.resolve(), args.out.resolve() / '.payload-building'
    if root.exists():
        raise PackError('output already exists; use a fresh directory')
    if METAL_FLAGS != ['-fmetal-math-mode=safe', '-fpreserve-invariance']:
        raise PackError(f'unexpected project compiler flags: {METAL_FLAGS!r}')
    chain = runtime_chain()
    requests, request_hashes = load_requests(args.requests)
    source_raw = (args.restore_from / 'seed-manifest.json').read_bytes()
    base_raw = args.base_manifest.read_bytes()
    source, baseline = json.loads(source_raw), json.loads(base_raw)
    if source.get('chain') != chain or baseline.get('chain') != chain:
        raise PackError('source/baseline translation chain differs from runtime')
    restore = variants(source) - variants(baseline)
    if len(requests) != args.expect_requests or len(restore) != args.expect_restore:
        raise PackError(f'count mismatch: {len(requests)} requests, {len(restore)} restored')
    if restore & requests.keys():
        raise PackError('restored variants overlap requests; verify source inventory')
    service = args.tree / 'build/shader-service/totk_xlate'
    spirv = args.tree / 'build/spirv-cross/spirv-cross'
    service_hash = file_digest(service)
    pin = (PROJECT / 'prebuilt/macos-arm64/totk_xlate.sha256').read_text().split()[0]
    if service_hash != pin:
        raise PackError('translator SHA-256 differs from the project pin')
    env = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    compiler = command(['xcrun', '-sdk', 'iphoneos', 'metal', '--version'], env).strip()
    root.mkdir(parents=True)
    output.mkdir()
    translate = root / 'translation'
    translate.mkdir()
    progress = root / 'progress.json'
    def status(phase, **values):
        progress.write_bytes(json_bytes(dict(phase=phase, elapsed_seconds=round(time.time()-started, 2), **values)))
        print(json.dumps(dict(phase=phase, **values)), flush=True)
    status('translating', requests=len(requests), restore=len(restore))
    with (root / 'translator.stderr.log').open('w') as errors:
        svc = subprocess.Popen([str(service), '--service', '--spirv-cross', str(spirv),
                                '--out', str(translate), '--chain', chain],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, text=True)
        try:
            for n, (variant, rq) in enumerate(requests.items(), 1):
                job = dict(rq)
                code = translate / (variant + '.code')
                code.write_bytes(bytes.fromhex(job.pop('code_hex')))
                job['code'] = str(code)
                svc.stdin.write(json.dumps(job, sort_keys=True) + '\n')
                svc.stdin.flush()
                line = svc.stdout.readline()
                if not line:
                    raise PackError(f'translator exited for {variant}')
                reply = json.loads(line)
                if not reply.get('ok'):
                    raise PackError(f'translator rejected {variant}: {reply.get("error")}')
                if n % 25 == 0:
                    status('translating', completed=n, total=len(requests))
        finally:
            svc.stdin.close()
            if svc.wait() != 0:
                raise PackError('translator exited with a failure')
    indexes, stages = collections.defaultdict(list), collections.Counter()
    source_indexes = {}
    for variant in sorted(restore | requests.keys()):
        origin = args.restore_from if variant in restore else translate
        for suffix in ('_b.metal', '_b.json'):
            name = variant + suffix
            path = origin / name
            if variant in restore and file_digest(path) != source['files'].get(name):
                raise PackError(f'restore source SHA-256 mismatch: {name}')
            shutil.copyfile(path, output / name)
        reflection = json.loads((output / (variant + '_b.json')).read_text())
        deps = reflection.get('deps', {})
        if reflection.get('chain') != chain or not isinstance(deps, dict):
            raise PackError(f'reflection chain/dependencies mismatch: {variant}')
        if variant in requests:
            if not deps_match(deps, requests[variant]):
                raise PackError(f'new variant does not serve its original iOS request: {variant}')
            stages[requests[variant]['stage']] += 1
        else:
            shader = variant.split('-')[0]
            if shader not in source_indexes:
                source_indexes[shader] = json.loads((args.restore_from / (shader + '.variants')).read_text())
            if not any(r.get('variant') == variant and r.get('deps') == deps for r in source_indexes[shader]):
                raise PackError(f'restored variant lacks source index with verified dependencies: {variant}')
        indexes[variant.split('-')[0]].append(dict(variant=variant, deps=deps))
    selected = sorted(restore | requests.keys())
    status('compiling', variants=len(selected), workers=args.workers, flags=METAL_FLAGS)
    def compile_one(variant):
        src, air, lib = [output / (variant + s) for s in ('_b.metal', '_b.air', '_b.metallib')]
        try:
            command(['xcrun', '-sdk', 'iphoneos', 'metal', *METAL_FLAGS, '-c', str(src), '-o', str(air)], env)
            command(['xcrun', '-sdk', 'iphoneos', 'metallib', str(air), '-o', str(lib)], env)
        finally:
            air.unlink(missing_ok=True)
        return dict(variant=variant, msl_sha256=file_digest(src), metallib_sha256=file_digest(lib),
                    target=library(lib), flags=METAL_FLAGS)
    compiled = []
    with (root / 'compile-receipt.jsonl').open('x') as log:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
            jobs = {pool.submit(compile_one, v): v for v in selected}
            for future in concurrent.futures.as_completed(jobs):
                result = future.result()
                compiled.append(result)
                log.write(json.dumps(result, sort_keys=True) + '\n')
                log.flush()
                if len(compiled) % 100 == 0 or len(compiled) == len(selected):
                    status('compiling', completed=len(compiled), total=len(selected))
    for shader, rows in sorted(indexes.items()):
        (output / (shader + '.variants')).write_bytes(json_bytes(rows))
    files = {path.name: file_digest(path) for path in sorted(output.iterdir()) if path.is_file()}
    manifest = dict(schema=1, chain=chain, shaders=len(selected), ios_metallibs=len(selected),
                    ios_metal_flags=' '.join(METAL_FLAGS), files=files)
    manifest_path = output / 'seed-manifest.json'
    manifest_path.write_bytes(json_bytes(manifest))
    normalized, payloads, report = prepare(output)
    if report['skipped_incomplete_variants'] or len(payloads) != len(files):
        raise PackError('runtime-contract normalization dropped payloads')
    # prepare() also attests target/chain/reflection and canonical index rows.
    if any((hashlib.sha256(v).hexdigest() if isinstance(v, bytes) else file_digest(v)) != files[n]
           for n, v in payloads.items()):
        raise PackError('runtime-contract normalization changed a payload')
    payload_bytes = sum(path.stat().st_size for path in output.iterdir())
    if payload_bytes > args.max_payload_mib * (1 << 20):
        raise PackError('payload exceeds the configured size budget')
    receipt = dict(schema=1, chain=chain, requests=len(requests), restored=len(restore),
                   variants=len(selected), runtime_files=len(files), payload_bytes=payload_bytes,
                   request_stages=dict(stages), request_hashes=request_hashes,
                   restored_variants=sorted(restore), requested_variants=sorted(requests),
                   source_manifest_sha256=hashlib.sha256(source_raw).hexdigest(),
                   baseline_manifest_sha256=hashlib.sha256(base_raw).hexdigest(),
                   translator_sha256=service_hash, spirv_cross_sha256=file_digest(spirv),
                   compiler=compiler, flags=METAL_FLAGS,
                   targets=dict(collections.Counter(r['target'] for r in compiled)),
                   manifest_sha256=file_digest(manifest_path), payload_sha256=files,
                   helper_sha256=file_digest(Path(__file__)),
                   importer_validation='pending: run actual native importer before device transfer',
                   elapsed_seconds=round(time.time()-started, 2))
    # The visible payload directory is the completion boundary for local callers.
    # A failed compile/verification leaves only the hidden staging directory.
    (root / 'preparation-receipt.json').write_bytes(json_bytes(receipt))
    output.rename(root / 'payload')
    status('prepared-not-transferred', variants=len(selected), files=len(files), payload_bytes=payload_bytes,
           manifest_sha256=receipt['manifest_sha256'])
    return receipt

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--requests', type=Path, required=True)
    parser.add_argument('--restore-from', type=Path, required=True)
    parser.add_argument('--base-manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--expect-requests', type=int, default=176)
    parser.add_argument('--expect-restore', type=int, default=1563)
    parser.add_argument('--max-payload-mib', type=int, default=300)
    args = parser.parse_args()
    if not 1 <= args.workers <= 8:
        parser.error('--workers must be between 1 and 8')
    try:
        build(args)
    except (OSError, ValueError, TypeError) as error:
        print(f'shader supplement preparation failed: {error}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
