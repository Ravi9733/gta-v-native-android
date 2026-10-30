#!/usr/bin/python3
"""Build only this private bridge and pinned SPIRV-Cross source, never the app."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = Path(os.environ.get('GTA_SPIRV_CROSS_SOURCE', Path(__file__).resolve().parents[3] / 'GTA_build/src/SPIRV-Cross'))
EXPECTED_COMMIT = 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165'
UNITS = ['spirv_cross.cpp', 'spirv_parser.cpp', 'spirv_cross_parsed_ir.cpp',
         'spirv_cfg.cpp', 'spirv_glsl.cpp', 'spirv_msl.cpp', 'spirv_reflect.cpp']
ENV = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')


def run(args, **kw):
    return subprocess.run(list(map(str, args)), env=ENV, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    commit = run(['git', '-C', SOURCE, 'rev-parse', 'HEAD']).stdout.decode().strip()
    dirty = run(['git', '-C', SOURCE, 'status', '--porcelain']).stdout.decode().strip()
    if commit != EXPECTED_COMMIT or dirty:
        raise SystemExit('Pinned SPIRV-Cross checkout changed or is dirty')
    clang = run(['xcrun', '--find', 'clang++']).stdout.decode().strip()
    ar = run(['xcrun', '--find', 'ar']).stdout.decode().strip()
    tasks, configs = [], {}
    for platform, sdk_name, target in [('ios-arm64', 'iphoneos', 'arm64-apple-ios26.0'),
                                       ('macos-arm64', 'macosx', 'arm64-apple-macos13.0')]:
        sdk = run(['xcrun', '--sdk', sdk_name, '--show-sdk-path']).stdout.decode().strip()
        directory = HERE/'build'/platform
        directory.mkdir(parents=True, exist_ok=True)
        flags = ['-std=c++17', '-O2', '-DNDEBUG', '-fPIC', '-fvisibility=hidden',
                 '-fvisibility-inlines-hidden', '-target', target, '-isysroot', sdk,
                 '-I', str(SOURCE), '-I', str(HERE)]
        configs[platform] = dict(target=target, sdk=sdk, flags=flags, objects=[])
        for source in [SOURCE/u for u in UNITS]+[HERE/'gta_spirv_cross.cpp']:
            output = directory/(source.stem+'.o')
            configs[platform]['objects'].append(str(output))
            tasks.append((platform, source, output, [clang, *flags, '-c', source, '-o', output]))
    def compile_one(task):
        platform, source, output, command = task
        try:
            run(command)
        except subprocess.CalledProcessError as exc:
            (output.parent/(source.stem+'.error.log')).write_bytes(exc.stderr)
            raise RuntimeError(platform+' '+source.name+': '+exc.stderr.decode(errors='replace')[:1600]) from exc
        print(platform, source.name, 'OK', flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile_one, tasks))
    artifacts = {}
    for platform, config in configs.items():
        directory = HERE/'build'/platform
        archive = directory/'libgta_spirv_cross.a'
        run([ar, 'rcs', archive, *config['objects']])
        artifacts[platform] = dict(archive=str(archive), archive_sha256=sha(archive))
        if platform == 'macos-arm64':
            library = directory/'libgta_spirv_cross.dylib'
            run([clang, '-dynamiclib', '-target', config['target'], '-isysroot', config['sdk'],
                 '-Wl,-install_name,@rpath/libgta_spirv_cross.dylib',
                 *config['objects'], '-o', library])
            artifacts[platform].update(dylib=str(library), dylib_sha256=sha(library))
    inputs = [HERE/'gta_spirv_cross.h', HERE/'gta_spirv_cross.cpp', HERE/'build.py']
    inputs += sorted(set([SOURCE/u for u in UNITS]+list(SOURCE.glob('*.hpp'))+list(SOURCE.glob('*.h'))))
    receipt = dict(source=str(SOURCE), pinned_commit=commit, source_dirty=False,
                   compiler=run([clang, '--version']).stdout.decode().splitlines()[0],
                   configs=configs, artifacts=artifacts,
                   input_sha256={str(q):sha(q) for q in inputs})
    if run(['git', '-C', SOURCE, 'status', '--porcelain']).stdout.strip():
        raise SystemExit('Source checkout unexpectedly changed during build')
    (HERE/'build-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(artifacts, indent=2))


if __name__ == '__main__':
    main()
