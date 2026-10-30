#!/usr/bin/env python3
"""Strict composition of the existing iOS wait patch and completion overflow fix.

Default is read-only. --apply atomically replaces only the prepared iOS text,
its digest headers and generated return-site header. Original Mac images stay
unchanged. Game files, phone data and signing are outside this tool's scope.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import re
import struct
import sys
import tempfile
import time

OLD_SHA='5251957ca949b2586b8db4e35daf139963fd3f5226d18d6bf793c75127d2c406'
NEW_SHA='1348a3bbe1745afbd3029327997c7acb728e4a7acadbdb751525b9b9a4ef2c3c'
MAC_SHA='463070253cdaa345f198146220c42a07fbdf1a8824ac74d1f3e69cdbaf02bb7b'
SDK_SHA='ec44b61ca87c461e702cbaa609f7b7c3736f570a3c0f58b6924590dde261f1c9'
WAIT_SOURCE_SHA='457b3c70d28ddb85affb4647a0ef50574c930e1f17ad596339bc8a98262f409e'
SIZE,CAVE=0x29f0000,0x29efea8
SITES=(('FULL',0x2655b7c,'44000014',40),
       ('POP_FAST',0x2669dd0,'b5bb00f0',48),
       ('POP_WAITING',0x2669e88,'cbae4ab9',48),
       ('POP_SIGNALED',0x2669f08,'cbae4ab9',48))
ROW=re.compile(rb'\{"main","(?P<x0>[0-9a-f]{64})","(?P<w>[0-9a-f]{64})","(?P<x1>[0-9a-f]{64})"\}')
def sha(b):return hashlib.sha256(b).hexdigest()
def word(w):return struct.pack('<I',w)
def branch(src,dst):
    d=dst-src
    if d%4 or not -(1<<27)<=d<(1<<27):raise ValueError('branch range')
    return word(0x14000000|((d//4)&0x3ffffff))
def cbnz19(src,dst):
    d=dst-src
    if d%4 or not -(1<<20)<=d<(1<<20):raise ValueError('CBNZ range')
    return word(0xb5000013|(((d//4)&0x7ffff)<<5))
def adrp(src,dst,reg):
    d=(dst>>12)-(src>>12)
    if not -(1<<20)<=d<(1<<20):raise ValueError('ADRP range')
    return word(0x90000000|((d&3)<<29)|(((d>>2)&0x7ffff)<<5)|reg)

def changes(sdk):
    if sha(sdk)!=SDK_SHA:raise ValueError('SDK gate template differs')
    template=sdk[0xc3e060:0xc3e084]
    if len(template)!=36:raise ValueError('SDK gate size')
    result=[];cave=CAVE
    for name,site,old,size in SITES:
        payload=bytearray(template);payload[8:12]=adrp(cave+8,0x3ef0000,29)
        if name=='FULL':payload+=branch(cave+36,0x2655c8c)
        else:
            empty={'POP_FAST':0x2669e64,'POP_WAITING':0x2669f00,'POP_SIGNALED':0x2669e74}[name]
            success=0x2669f9c if name=='POP_WAITING' else 0x2669fb4
            payload+=cbnz19(cave+36,cave+44)+branch(cave+40,empty)+branch(cave+44,success)
        if len(payload)!=size:raise ValueError('cave length')
        result.extend(((site,bytes.fromhex(old),branch(site,cave)),(cave,bytes(size),bytes(payload))))
        cave+=size
    if cave!=CAVE+184 or cave>SIZE:raise ValueError('cave budget')
    return result

def normalize(data,sdk):
    if len(data)!=SIZE or sha(data)!=NEW_SHA:raise ValueError('unknown overflow image')
    out=bytearray(data)
    for at,old,new in changes(sdk):
        if out[at:at+len(new)]!=new:raise ValueError('overflow patch bytes differ')
        out[at:at+len(old)]=old
    out=bytes(out)
    if sha(out)!=OLD_SHA:raise ValueError('normalization differs')
    return out

def transform(data,sdk,wait):
    if sha(data)==NEW_SHA:data=normalize(data,sdk)
    composed,_=wait.transform(data)
    if sha(composed)!=OLD_SHA or composed[CAVE:]!=bytes(SIZE-CAVE):raise ValueError('wait composition differs')
    out=bytearray(composed)
    for at,old,new in changes(sdk):
        if out[at:at+len(old)]!=old:raise ValueError('source instruction differs')
        out[at:at+len(new)]=new
    out=bytes(out)
    if sha(out)!=NEW_SHA:raise ValueError('overflow composition digest differs')
    return out

def header(data,wait):
    rows=list(ROW.finditer(data))
    if len(rows)!=1:raise ValueError('one main digest row required')
    row=rows[0]
    if row['x0'].decode()==NEW_SHA:data=data[:row.start('x0')]+OLD_SHA.encode()+data[row.end('x0'):]
    data=wait.header(data);row=list(ROW.finditer(data))[0]
    if row['x0'].decode()!=OLD_SHA:raise ValueError('wait digest header differs')
    return data[:row.start('x0')]+NEW_SHA.encode()+data[row.end('x0'):]

def sites_header():
    rows=['#ifndef GTA_COMPLETION_OVERFLOW_SITES_H','#define GTA_COMPLETION_OVERFLOW_SITES_H',
          '#define GTA_OVERFLOW_IMAGE_SHA256 "'+NEW_SHA+'"'];cave=CAVE
    for name,_,_,size in SITES:
        rows.append('#define GTA_OVERFLOW_'+name+'_RET '+hex(cave+32)+'ULL');cave+=size
    return ('\n'.join(rows+['#endif'])+'\n').encode()

def replace(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent,prefix='.'+path.name+'.',delete=False) as f:
        temp=Path(f.name);os.fchmod(f.fileno(),path.stat().st_mode&0o777 if path.exists() else 0o600)
        f.write(data);f.flush();os.fsync(f.fileno())
    try:os.replace(temp,path)
    finally:temp.unlink(missing_ok=True)

def load_wait(root):
    path=root/'ios/patch_gta_completion_wait.py'
    if path.is_symlink() or sha(path.read_bytes())!=WAIT_SOURCE_SHA:raise ValueError('wait composer source differs')
    spec=importlib.util.spec_from_file_location('gta_pinned_wait_composer',path)
    module=importlib.util.module_from_spec(spec);prior=sys.dont_write_bytecode;sys.dont_write_bytecode=True
    try:spec.loader.exec_module(module)
    finally:sys.dont_write_bytecode=prior
    if module.NEW_SHA!=OLD_SHA or module.SIZE!=SIZE:raise ValueError('wait composer identity differs')
    return module

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    ap.add_argument('--tree',type=Path);ap.add_argument('--gen-dir',type=Path)
    ap.add_argument('--apply',action='store_true');a=ap.parse_args()
    root=a.root.resolve();tree=(a.tree or root/'GTA_build').resolve();gen=(a.gen_dir or root/'ios/Gen').resolve()
    wait=load_wait(root);mac=tree/'rewritten/local/main.patched.bin';sdk_path=tree/'rewritten/local/sdk.patched.bin'
    layout=json.loads((tree/'analysis/runtime-layout.json').read_bytes())
    main_layout=next(row for row in layout if row['name']=='main')['layout']
    if main_layout['TEXT_END']!=0x29efe50 or main_layout['RO_OFF']!=SIZE:raise ValueError('main runtime layout differs')
    if sha(mac.read_bytes())!=MAC_SHA:raise ValueError('Mac rewrite differs')
    sdk=sdk_path.read_bytes();image=tree/'ios-embed/main.x0.bin'
    paths=[image,tree/'ios-embed/ios_embed.h',gen/'ios_embed.h']
    if any(p.is_symlink() for p in paths):raise ValueError('symlink input')
    before={p:p.read_bytes() for p in paths};plan={image:transform(before[image],sdk,wait)}
    for p in paths[1:]:plan[p]=header(before[p],wait)
    generated=gen/'completion_overflow_sites.h'
    if generated.is_symlink():raise ValueError('symlink generated header')
    if generated.exists():
        before[generated]=generated.read_bytes()
        if before[generated]!=sites_header():raise ValueError('existing generated sites differ')
    plan[generated]=sites_header()
    assemblies=[tree/'ios-embed/gen',gen/'embed.S']
    if any(p.is_symlink() or not p.is_file() for p in assemblies):raise ValueError('embedding assembly missing')
    if a.apply:
        if any(p.read_bytes()!=b for p,b in before.items()):raise ValueError('concurrent source edit')
        for p,b in plan.items():
            if not p.exists() or p.read_bytes()!=b:replace(p,b)
        # .incbin dependencies are not tracked by the compiler. Never regress
        # either assembly timestamp, including an earlier future-dated build.
        fresh=max(time.time_ns(),max(p.stat().st_mtime_ns for p in [*plan,*assemblies])+1)
        for p in assemblies:os.utime(p,ns=(p.stat().st_atime_ns,fresh))
        if any(p.read_bytes()!=b for p,b in plan.items()):raise ValueError('post-apply readback')
        if sha(mac.read_bytes())!=MAC_SHA or sha(sdk_path.read_bytes())!=SDK_SHA:raise ValueError('protected Mac image changed')
    print(json.dumps({'mode':'apply' if a.apply else 'check','source_sha256':sha(before[image]),
        'new_sha256':NEW_SHA,'changed':[str(p) for p,b in plan.items() if before.get(p)!=b],
        'semantics':'native completed-payload overflow; original decrement/dispatch/wake paths',
        'phone_accessed':False},indent=2))

if __name__=='__main__':main()
