#!/usr/bin/env python3
"""Private mechanically adapted prototype; never edits pinned production files."""
from pathlib import Path
import hashlib, json, shutil

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent / 'translator'
SOURCE = ROOT / 'tooling/totk_xlate'
VENDOR = ROOT / 'tooling/ryujinx'
OUT.mkdir(exist_ok=True)
receipt = {'schema': 1, 'chain': 'totk-native-v4-unverified', 'source_files': {}, 'adaptations': []}
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def pin(p): receipt['source_files'][str(p.relative_to(ROOT))] = digest(p)
for name in ('Program.cs', 'Service.cs', 'VertexCounterLowering.cs', 'totk_xlate.csproj'):
    pin(SOURCE / name)
program = (SOURCE / 'Program.cs').read_text()
service = (SOURCE / 'Service.cs').read_text()
assert program.count('static class SwshXlate') == 1
assert service.count('static class ShaderService') == 1
assert service.count('static void SpirvCross(') == 1
assert program.count('    // --disasm') == 1
assert service.count('    // totk_xlate --service') == 1
# The native ABI never enters CLI/disassembler modes. They contain reflection,
# environment/console handling unrelated to the pinned transformation chain.
program = program[:program.index('    // --disasm')] + '}\n'
service = service[:service.index('    // totk_xlate --service')] + '}\n'
service = service.replace('static string spirvCross, outDir, chain;', 'static string outDir, chain;')
start = service.index('    static void SpirvCross(')
end = service.index('\n    static void Translate(', start)
service = service[:start] + '''    static void SpirvCross(string spv, string msl, string what)
    {
        try { MemoryFiles.WriteAllText(msl, NativeCross.ToMsl(MemoryFiles.ReadAllBytes(spv))); }
        catch (Exception e) { throw new RuntimeError(what + e.Message); }
    }
''' + service[end:]
service = service.replace('static class ShaderService', 'static partial class ShaderService', 1)
service = service.replace('using System.Diagnostics;\n', '')
for name, text in (('Program.Generated.cs', program), ('Service.Generated.cs', service)):
    text = text.replace('File.', 'MemoryFiles.').replace('Directory.CreateDirectory(', 'MemoryFiles.CreateDirectory(')
    # Replacement also sees the already adapted MemoryFiles call; normalize it.
    text = text.replace('MemoryMemoryFiles.', 'MemoryFiles.')
    assert 'Process.Start' not in text and 'ProcessStartInfo' not in text
    (OUT / name).write_text(text)
shutil.copy2(SOURCE / 'VertexCounterLowering.cs', OUT / 'VertexCounterLowering.cs')
receipt['adaptations'] = [
    'All Program/Service File calls redirected to per-call in-memory map; no host file API.',
    'Directory.CreateDirectory redirected to no-op in-memory implementation.',
    'Only Service.SpirvCross body replaced with registered unmanaged C ABI callback.',
    'ShaderService made partial for a serialized exported in-memory adapter.',
    'Unreachable CLI Main/Run and reflection-only Disasm removed from generated library source.',
    'Service.Handle, shader transformations/reflection, and VertexCounterLowering preserved.',
]
# Isolated source references ensure build outputs do not touch production vendor.
vendor_out = OUT / 'vendor'
for name in ('Directory.Build.props', 'Directory.Packages.props'):
    pin(VENDOR / name)
    vendor_out.mkdir(exist_ok=True)
    shutil.copy2(VENDOR / name, vendor_out / name)
for name in ('Ryujinx.Common', 'Ryujinx.Graphics.Shader', 'Spv.Generator'):
    src = VENDOR / 'src' / name
    for p in sorted(src.rglob('*')):
        if p.is_file() and not any(x in ('bin', 'obj') for x in p.relative_to(src).parts): pin(p)
    dst = vendor_out / 'src' / name
    if dst.exists():
        for p in src.rglob('*'):
            if p.is_file() and not any(x in ('bin', 'obj') for x in p.relative_to(src).parts) and not p.name.startswith('._'):
                assert digest(p) == digest(dst / p.relative_to(src)), f'private vendor drift: {p}'
    else:
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns('bin', 'obj', '._*'))
receipt['generated_files'] = {p.name: digest(p) for p in sorted(OUT.glob('*.cs'))}
(OUT.parent / 'source-receipt.json').write_text(json.dumps(receipt, indent=2, sort_keys=True)+'\n')
print(f'Generated private source copy; {len(receipt["source_files"])} pinned input files.')
