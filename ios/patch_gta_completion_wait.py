#!/usr/bin/env python3
"""Pinned, offline iOS-only completion-queue wait mitigation. Default: no writes.

Only --apply changes the generated main.x0 + its two digest headers and touches
the two embedding assembly files. This is not a general Semaphore/HLE change.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time

ROOT = (Path(__file__).resolve().parents[1] if Path(__file__).parent.name == 'ios'
        else Path(__file__).resolve().parents[1])
WORKER_SOURCE_SHA = '995f4d782e67441c5d80622046a1db10015efc92c389e1e5ee378e2eea18615d'
OLD_SHA = '8f229188029e43a828c839daaab3160019efeb1d32d1c436cf5fd5a260e88ad2'
REWRITE_SHA = '463070253cdaa345f198146220c42a07fbdf1a8824ac74d1f3e69cdbaf02bb7b'
W_SHA = 'ebad646538dfcff8f18359f7f6ea23b9acab31ad83d44d19fcf02d2878f3a376'
X1_SHA = '7af1163b40b5b431d31d11b1a547be9572a2122452b696ce03b7eb51ffb50817'
SIZE = 0x29F0000
SITE = 0x2669F04
CAVE = 0x29EFE90
TIMEOUT_NS = 100_000_000
NEW_SHA = '5251957ca949b2586b8db4e35daf139963fd3f5226d18d6bf793c75127d2c406'
ROW = re.compile(rb'\{"main","(?P<x0>[0-9a-f]{64})","(?P<w>[0-9a-f]{64})","(?P<x1>[0-9a-f]{64})"\}')

def sha(b): return hashlib.sha256(b).hexdigest()

def branch(src, dst, link=False):
    d = dst-src
    if d % 4 or not -(1<<27) <= d < (1<<27): raise ValueError('B/BL range')
    return struct.pack('<I', (0x94000000 if link else 0x14000000) | ((d//4)&0x3ffffff))

def cbnz_w0(src, dst):
    d = dst-src
    if d % 4 or not -(1<<20) <= d < (1<<20): raise ValueError('CBZ range')
    return struct.pack('<I', 0x35000000 | (((d//4)&0x7ffff)<<5))

PAYLOAD = (struct.pack('<I', 0xD2800001 | ((TIMEOUT_NS&0xffff)<<5)) +
           struct.pack('<I', 0xF2A00001 | (((TIMEOUT_NS>>16)&0xffff)<<5)) +
           branch(CAVE+8, 0x29EF380, True) +
           cbnz_w0(CAVE+12, CAVE+20) +
           branch(CAVE+16, 0x2669E88) +
           branch(CAVE+20, 0x2669F08))
CHANGES = ((SITE, bytes.fromhex('1b150e94'), branch(SITE,CAVE)),
           (CAVE, bytes(len(PAYLOAD)), PAYLOAD))
# Pin both existing caves as well as the entire original image hash. They are
# earlier independent fixes and must remain byte-for-byte intact.
EARLIER_CAVES = bytes.fromhex(
    '88098352bf6b283813f6f51700000000'
    'c80e148b092d40f9e827bfa9132d00f944ba5897e827c1a8'
    '40000035092d00f9beb0feb0deb300916950581700000000')

def worker_module(root=ROOT):
    path=root/'ios/patch_gta_worker_publish.py'
    if path.is_symlink() or sha(path.read_bytes())!=WORKER_SOURCE_SHA:
        raise ValueError('Pinned worker-publish source changed')
    spec=importlib.util.spec_from_file_location('gta_pinned_worker_publish',path)
    module=importlib.util.module_from_spec(spec)
    prior=sys.dont_write_bytecode;sys.dont_write_bytecode=True
    try:spec.loader.exec_module(module)
    finally:sys.dont_write_bytecode=prior
    if (module.COMPOSED_SHA,module.IMAGE_SIZE)!=(OLD_SHA,SIZE):
        raise ValueError('Worker identities changed')
    return module

def remove_own_patch(data):
    if len(data)!=SIZE or sha(data)!=NEW_SHA:raise ValueError('Only exact final image can be normalized')
    out=bytearray(data)
    for off,old,new in CHANGES:
        if data[off:off+len(new)]!=new:raise ValueError('Own patch bytes changed')
        out[off:off+len(old)]=old
    out=bytes(out)
    if sha(out)!=OLD_SHA:raise ValueError('Normalization did not recover composed image')
    return out

def transform(data,worker=None):
    worker=worker or worker_module()
    digest = sha(data)
    states={worker.BASE_SHA:'base',worker.READY_SHA:'async_ready',OLD_SHA:'composed',NEW_SHA:'already_final'}
    if len(data) != SIZE or digest not in states:
        raise ValueError(f'Unknown image size/hash: {len(data)} / {digest}')
    normalized=remove_own_patch(data) if digest==NEW_SHA else data
    composed,_=worker.transform_image(normalized)
    if sha(composed)!=OLD_SHA:raise ValueError('Worker composition identity changed')
    if composed[0x29EFE50:CAVE] != EARLIER_CAVES: raise ValueError('Existing caves changed')
    if composed[CAVE:] != bytes(SIZE-CAVE):
        raise ValueError('Nonzero remaining RX padding')
    for off,old,new in CHANGES:
        if composed[off:off+len(old)] != old:
            raise ValueError(f'Unexpected instruction at {off:#x}')
    out=bytearray(composed)
    for off,old,new in CHANGES: out[off:off+len(old)]=new
    out=bytes(out)
    if sha(out) != NEW_SHA: raise ValueError('Candidate hash mismatch')
    return out, states[digest]

def header(data,worker=None):
    worker=worker or worker_module()
    rows=list(ROW.finditer(data))
    if len(rows)!=1: raise ValueError('Expected one complete main digest row')
    r=rows[0]
    if r['x0'].decode() not in (worker.BASE_SHA,worker.READY_SHA,OLD_SHA,NEW_SHA) or r['w'].decode()!=W_SHA or r['x1'].decode()!=X1_SHA:
        raise ValueError('Unexpected main digest tuple')
    normalized=(data[:r.start('x0')]+OLD_SHA.encode()+data[r.end('x0'):]) if r['x0'].decode()==NEW_SHA else data
    composed,_=worker.transform_header(normalized)
    r=list(ROW.finditer(composed))[0]
    return composed[:r.start('x0')]+NEW_SHA.encode()+composed[r.end('x0'):]

def atomic_write(path,data):
    with tempfile.NamedTemporaryFile(dir=path.parent,prefix='.'+path.name+'.',delete=False) as f:
        temp=Path(f.name)
        os.fchmod(f.fileno(),path.stat().st_mode&0o777)
        f.write(data); f.flush(); os.fsync(f.fileno())
    try: os.replace(temp,path)
    finally: temp.unlink(missing_ok=True)

def self_test(original,headers,worker):
    candidate,_=transform(original,worker)
    composed=remove_own_patch(candidate);ready=worker.remove_worker(composed)
    base=worker.async_module().original_from_patched(ready)
    for image in [base,ready,composed,candidate]:assert transform(image,worker)[0]==candidate
    assert transform(candidate,worker)==(candidate,'already_final')
    for off in [0,SITE,CAVE,0x29EFE50,SIZE-1]:
        bad=bytearray(composed);bad[off]^=1
        try: transform(bytes(bad),worker)
        except ValueError: pass
        else: raise AssertionError('Unknown image accepted')
    for h in headers:
        assert header(header(h,worker),worker)==header(h,worker)
        for digest in [worker.BASE_SHA,worker.READY_SHA,OLD_SHA,NEW_SHA]:
            m=list(ROW.finditer(h))[0]
            version=h[:m.start('x0')]+digest.encode()+h[m.end('x0'):]
            assert header(version,worker)==header(h,worker)
        for bad in [h+h,h.replace(W_SHA.encode(),b'0'*64)]:
            try:header(bad,worker)
            except ValueError:pass
            else:raise AssertionError('Unknown header accepted')
    return {'image_states':4,'header_states':4,'unknown_images_rejected':5,
            'invalid_headers_rejected':4,'image_and_header_idempotent':True}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,default=ROOT)
    ap.add_argument('--tree',type=Path)
    mode=ap.add_mutually_exclusive_group()
    mode.add_argument('--apply',action='store_true');mode.add_argument('--check',action='store_true')
    ap.add_argument('--self-test',action='store_true')
    a=ap.parse_args();r=a.root.resolve();tree=a.tree.resolve() if a.tree else r/'GTA_build'
    worker=worker_module(r)
    image=tree/'ios-embed/main.x0.bin'
    headers=[tree/'ios-embed/ios_embed.h',r/'ios/Gen/ios_embed.h']
    assemblies=[tree/'ios-embed/gen',r/'ios/Gen/embed.S']
    rewrite=tree/'rewritten/local/main.patched.bin'
    if sha(rewrite.read_bytes())!=REWRITE_SHA:raise ValueError('Mac rewrite changed')
    layout=json.loads((tree/'analysis/runtime-layout.json').read_bytes())
    ml=next(x for x in layout if x['name']=='main')['layout']
    if ml['TEXT_END']!=0x29EFE50 or ml['RO_OFF']!=SIZE:raise ValueError('Main layout changed')
    protected={p:sha(p.read_bytes()) for p in [rewrite,r/'ios/patch_gta_worker_publish.py',r/'ios/patch_gta_async_ready.py']}
    native=tree/'build/gta-native'
    if native.exists():protected[native]=sha(native.read_bytes())
    originals={p:p.read_bytes() for p in [image,*headers]}
    if any(p.is_symlink() for p in [image,*headers,*assemblies]):raise ValueError('Symlink rejected')
    out,state=transform(originals[image],worker);planned={image:out}
    planned.update({p:header(originals[p],worker) for p in headers})
    tests=self_test(originals[image],[originals[p] for p in headers],worker) if a.self_test else None
    changed=[p for p in planned if planned[p]!=originals[p]]
    if a.apply:
        if any(p.read_bytes()!=b for p,b in originals.items()):raise ValueError('Concurrent input edit')
        for p in changed:atomic_write(p,planned[p])
        # Both dependency files are mandatory: builder copies the generated one.
        fresh=max(time.time_ns(),max(p.stat().st_mtime_ns for p in planned)+1)
        for p in assemblies:os.utime(p,ns=(p.stat().st_atime_ns,fresh))
        if any(p.read_bytes()!=b for p,b in planned.items()):raise ValueError('Readback mismatch')
        if any(sha(p.read_bytes())!=digest for p,digest in protected.items()):raise ValueError('Protected Mac/scripts changed during apply')
    print(json.dumps({'state':state,'mode':'apply' if a.apply else 'check','old_sha256':OLD_SHA,
      'new_sha256':NEW_SHA,'site':hex(SITE),'cave':hex(CAVE),'timeout_ns':TIMEOUT_NS,
      'changed':[str(p) for p in changed],'self_test_assertions':tests,
      'semantics':'TimedAcquire true -> original postwait retry; false -> existing postflag retry/cleanup',
      'scope':'offline generated iOS main only; no host semaphore change; no device calls'},indent=2))

if __name__=='__main__':main()
