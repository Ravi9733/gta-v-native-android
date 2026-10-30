#!/usr/bin/env python3
"""Strict optional iOS job ledger after pinned overflow/wait fixes.

Default read-only. --apply writes prepared iOS image/digest/site headers and
advances .incbin dependency timestamps. Default produces clean1348 image even
from a previous380d diagnostic image. --job-ledger explicitly produces380d.
No phone, signing or Mac-image changes.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,os,struct,sys,time

OLD_SHA='1348a3bbe1745afbd3029327997c7acb728e4a7acadbdb751525b9b9a4ef2c3c'
NEW_SHA='380df2871466aed464af6b66e116e9ddf8fe52300c962b24dd9adf9316fcdbb2'
PREVIOUS_SOURCE_SHA='ab72ff668efe36ed3f18cfec9db1a5aa9bea29bff07421157261845dbf4b27a1'
SIZE,CAVE,KEY=0x29f0000,0x29eff60,0x3ef0000
SITES=(('COUNTED_SUBMIT',0x2525330,'070300b4',0x29eff60,0x29eff80,48),
       ('EXECUTE',0x229512c,'e00314aa',0x29eff90,0x29effb0,44),
       ('OUTER_DECREMENT',0x2525424,'e9070132',0x29effbc,0x29effdc,44))
sha=lambda b:hashlib.sha256(b).hexdigest()
word=lambda n:struct.pack('<I',n)
def branch(src,dst):
    d=dst-src
    if d%4 or not -(1<<27)<=d<(1<<27):raise ValueError('branch range')
    return word(0x14000000|((d//4)&0x3ffffff))
def cbz_x(src,dst,reg):
    d=dst-src
    if d%4 or not -(1<<20)<=d<(1<<20):raise ValueError('cbz range')
    return word(0xb4000000|(((d//4)&0x7ffff)<<5)|reg)
def adrp(src,dst,reg):
    d=(dst>>12)-(src>>12)
    if not -(1<<20)<=d<(1<<20):raise ValueError('adrp range')
    return word(0x90000000|((d&3)<<29)|(((d>>2)&0x7ffff)<<5)|reg)
def gate(at):
    return b''.join((word(0xa9bf7bfd),word(0xd53bd07e),adrp(at+8,KEY,29),
       word(0xf9400fbd),word(0x8b1d03de),word(0xf94003de),word(0xf94003fd),
       word(0xd63f03c0),word(0xa8c17bfd)))
def changes():
    result=[];end=CAVE
    for name,site,old,at,ret,size in SITES:
        if at!=end or ret!=at+32:raise ValueError('site layout')
        payload=gate(at)
        if name=='COUNTED_SUBMIT':payload+=cbz_x(at+36,at+44,7)+branch(at+40,site+4)+branch(at+44,0x2525390)
        else:payload+=bytes.fromhex(old)+branch(at+40,site+4)
        if len(payload)!=size:raise ValueError('veneer size')
        result.extend(((site,bytes.fromhex(old),branch(site,at)),(at,bytes(size),payload)));end+=size
    if end!=SIZE-24:raise ValueError('cave budget')
    return result
def normalize(data):
    if len(data)!=SIZE or sha(data)!=NEW_SHA:raise ValueError('unknown ledger image')
    out=bytearray(data)
    for at,old,new in changes():
        if out[at:at+len(new)]!=new:raise ValueError('ledger patch bytes')
        out[at:at+len(old)]=old
    out=bytes(out)
    if sha(out)!=OLD_SHA:raise ValueError('ledger normalization')
    return out
def transform(data,sdk,previous,wait,job_ledger=False):
    if sha(data)==NEW_SHA:data=normalize(data)
    old=previous.transform(data,sdk,wait)
    if sha(old)!=OLD_SHA or old[CAVE:]!=bytes(SIZE-CAVE):raise ValueError('previous composition differs')
    if not job_ledger:return old
    out=bytearray(old)
    for at,before,after in changes():
        if out[at:at+len(before)]!=before:raise ValueError('ledger before bytes')
        out[at:at+len(after)]=after
    out=bytes(out)
    if sha(out)!=NEW_SHA:raise ValueError('ledger composition digest')
    return out
def header(data,previous,wait,job_ledger=False):
    rows=list(previous.ROW.finditer(data))
    if len(rows)!=1:raise ValueError('one complete main digest row required')
    row=rows[0]
    if row['x0'].decode()==NEW_SHA:data=data[:row.start('x0')]+OLD_SHA.encode()+data[row.end('x0'):]
    result=previous.header(data,wait);row=list(previous.ROW.finditer(result))[0]
    if row['x0'].decode()!=OLD_SHA:raise ValueError('previous header digest')
    if not job_ledger:return result
    return result[:row.start('x0')]+NEW_SHA.encode()+result[row.end('x0'):]
def sites_header():
    lines=['#pragma once','#define GTA_JOB_LEDGER_IMAGE_SHA256 "'+NEW_SHA+'"']
    for name,site,_,_,ret,_ in SITES:
        lines+=['#define GTA_JOB_'+name+'_RET '+hex(ret)+'ULL','#define GTA_JOB_'+name+'_SITE '+hex(site)+'U']
    return ('\n'.join(lines)+'\n').encode()
def overflow_header(previous,job_ledger=False):
    data=previous.sites_header()
    if data.count(OLD_SHA.encode())!=1:raise ValueError('previous overflow header')
    return data.replace(OLD_SHA.encode(),NEW_SHA.encode()) if job_ledger else data
def load_previous(root):
    path=root/'ios/patch_gta_completion_overflow.py'
    if path.is_symlink() or sha(path.read_bytes())!=PREVIOUS_SOURCE_SHA:raise ValueError('previous composer source differs')
    spec=importlib.util.spec_from_file_location('gta_pinned_overflow_for_ledger',path)
    module=importlib.util.module_from_spec(spec);prior=sys.dont_write_bytecode;sys.dont_write_bytecode=True
    try:spec.loader.exec_module(module)
    finally:sys.dont_write_bytecode=prior
    if module.NEW_SHA!=OLD_SHA or module.SIZE!=SIZE:raise ValueError('previous composer identity')
    return module
def plan(root,tree,gen,job_ledger=False):
    previous=load_previous(root);wait=previous.load_wait(root)
    mac=tree/'rewritten/local/main.patched.bin';sdk_path=tree/'rewritten/local/sdk.patched.bin'
    if sha(mac.read_bytes())!=previous.MAC_SHA:raise ValueError('Mac rewrite differs')
    sdk=sdk_path.read_bytes()
    if sha(sdk)!=previous.SDK_SHA:raise ValueError('SDK differs')
    ml=next(row for row in json.loads((tree/'analysis/runtime-layout.json').read_bytes()) if row['name']=='main')['layout']
    if ml['TEXT_END']!=0x29efe50 or ml['RO_OFF']!=SIZE:raise ValueError('main runtime layout differs')
    image=tree/'ios-embed/main.x0.bin';headers=[tree/'ios-embed/ios_embed.h',gen/'ios_embed.h']
    assemblies=[tree/'ios-embed/gen',gen/'embed.S']
    for path in [image,*headers,*assemblies]:
        if path.is_symlink() or not path.is_file():raise ValueError('symlink/missing input')
    before={path:path.read_bytes() for path in [image,*headers]}
    output={image:transform(before[image],sdk,previous,wait,job_ledger)}
    output.update({path:header(before[path],previous,wait,job_ledger) for path in headers})
    oldh=previous.sites_header();diagh=overflow_header(previous,True)
    # A clean build removes only the exact generated diagnostic header. Unknown
    # or symlink headers are rejected, never silently deleted. All host uses
    # are compiled out without GTA_JOB_LEDGER_ENABLE=1.
    generated={gen/'completion_overflow_sites.h':(diagh if job_ledger else oldh,(oldh,diagh)),
               gen/'gta_job_ledger_sites.h':(sites_header() if job_ledger else None,(sites_header(),))}
    for path,(data,accepted) in generated.items():
        if path.is_symlink():raise ValueError('symlink generated sites')
        if path.exists():
            before[path]=path.read_bytes()
            if before[path] not in accepted:raise ValueError('existing generated sites differ')
        output[path]=data
    return previous,before,output,assemblies,(mac,sdk_path)
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);ap.add_argument('--tree',type=Path)
    ap.add_argument('--gen-dir',type=Path);ap.add_argument('--apply',action='store_true')
    ap.add_argument('--job-ledger',action='store_true',help='opt in to diagnostic guest job gates')
    a=ap.parse_args()
    root=a.root.resolve();tree=(a.tree or root/'GTA_build').resolve();gen=(a.gen_dir or root/'ios/Gen').resolve()
    previous,before,output,assemblies,protected=plan(root,tree,gen,a.job_ledger)
    protected_before={path:sha(path.read_bytes()) for path in protected}
    if a.apply:
        if any(path.read_bytes()!=data for path,data in before.items()):raise ValueError('concurrent source edit')
        # Include old future-dated assembly/generated inputs before deletion,
        # then include replacement mtimes again before advancing .incbin refs.
        reference=max(time.time_ns(),max(path.stat().st_mtime_ns for path in [*output,*assemblies] if path.exists())+1)
        for path,data in output.items():
            if path not in before and path.exists():raise ValueError('concurrent generated output')
            if data is None:
                if path.exists():
                    if path.read_bytes()!=before[path]:raise ValueError('concurrent diagnostic header edit')
                    path.unlink()
            elif not path.exists() or path.read_bytes()!=data:previous.replace(path,data)
        fresh=max(reference,max(path.stat().st_mtime_ns for path in [*output,*assemblies] if path.exists())+1)
        for path in assemblies:os.utime(path,ns=(path.stat().st_atime_ns,fresh))
        if any((path.read_bytes() if path.exists() else None)!=data for path,data in output.items()):raise ValueError('post-apply readback')
        if any(sha(path.read_bytes())!=digest for path,digest in protected_before.items()):raise ValueError('protected rewrite changed')
    print(json.dumps({'mode':'apply' if a.apply else 'check','source_sha256':sha(next(iter(before.values()))),'new_sha256':NEW_SHA if a.job_ledger else OLD_SHA,
      'changed':[str(path) for path,data in output.items() if before.get(path)!=data],
      'job_ledger':a.job_ledger,'recording_default':'OFF; explicit manual arm after startup' if a.job_ledger else 'compiled out; no guest job gates','queue_semantics_changed':False,
      'phone_accessed':False},indent=2))
if __name__=='__main__':main()
