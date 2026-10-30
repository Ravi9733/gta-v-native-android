#!/usr/bin/env python3
"""Build an unsigned GTA V iPhone/iPad app from the prepared GTA work tree.
Adapted from games/totk/ios/tools/build_ipa.py. No RomFS: the game's .rpf tree lives in
the app's Documents/GameData (copied to the device by install_device.py)."""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
TOTK_IOS = Path.home()/'Games/MacNativeGames/games/totk/ios/tools'

def run(args, cwd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.run([str(x) for x in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-30:]
        raise RuntimeError(f'{args[0]} failed; see {log}\n' + '\n'.join(tail))

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--tree', type=Path, default=HERE/'GTA_build')
    p.add_argument('--out', type=Path, default=HERE/'GTA-iPhone.app')
    p.add_argument('--logs', type=Path, default=HERE/'GTA_build/logs/ios')
    p.add_argument('--app-name', default='GTA V')
    p.add_argument('--bundle-id', default='org.macnativegames.gta')
    p.add_argument('--job-ledger', action='store_true', help='diagnostic job instrumentation; default is a clean performance image')
    a = p.parse_args()
    tree = a.tree.resolve(); ios = HERE/'ios'; gen = ios/'Gen'; logs = a.logs.resolve(); out = a.out.resolve()
    developer_dir = Path('/Applications/Xcode.app/Contents/Developer')
    if not (developer_dir/'Platforms/iPhoneOS.platform').is_dir():
        raise RuntimeError('full Xcode with the iOS platform required: /Applications/Xcode.app')
    env = dict(os.environ, DEVELOPER_DIR=str(developer_dir))
    subprocess.run(['xcrun','--sdk','iphoneos','--show-sdk-path'], env=env, check=True, stdout=subprocess.DEVNULL)
    if not shutil.which('xcodegen'):
        raise RuntimeError('install xcodegen: brew install xcodegen')
    gen.mkdir(parents=True, exist_ok=True)
    # generated loader tables (from prepare_gta.py on the Mac)
    for name in ('runtime_tables.h','romfs_config.h','shared_fonts_config.h'):
        shutil.copy2(tree/'build'/name, gen/name)
    # exported + embedded module images
    modules = tree/'ios-embed'
    shutil.copy2(modules/'gen', gen/'embed.S')
    shutil.copy2(modules/'ios_embed.h', gen/'ios_embed.h')
    # Always compose ordering/wait/overflow fixes; job gates are explicit --job-ledger only.
    # The strict wrapper validates the pinned iOS images; the Mac rewrite stays intact.
    run([sys.executable, ios/'patch_gta_job_ledger.py', '--tree', tree, '--apply'] +
        (['--job-ledger'] if a.job_ledger else []),
        ios, logs/'ios-ready-order-patch.log', env)
    # embed linker flags
    flags = (modules/'embed-flags.txt').read_text().strip()
    xcconfig = gen/'totk.xcconfig'
    xcconfig.write_text(
        f'TOTK_APP_NAME = {a.app_name}\n'
        f'TOTK_BUNDLE_ID = {a.bundle_id}\n'
        f'OTHER_LDFLAGS = $(inherited) {flags}\n'
        f'CODE_SIGNING_ALLOWED = NO\n'
        f'GCC_PREPROCESSOR_DEFINITIONS = $(inherited) GTA_JOB_LEDGER_ENABLE={1 if a.job_ledger else 0}\n')
    # iOS audio/VP9 dependencies (opus, libvpx for arm64-ios)
    deps = tree/'build/ios-deps'
    if not ((deps/'lib/libopus.a').is_file() and (deps/'lib/libvpx.a').is_file()):
        run([sys.executable, TOTK_IOS/'build_deps.py', deps], tree, logs/'ios-dependencies.log', env)
    shutil.rmtree(gen/'deps', ignore_errors=True)
    shutil.copytree(deps/'include', gen/'deps/include', dirs_exist_ok=True)
    (gen/'deps/lib').mkdir(parents=True, exist_ok=True)
    for lib in ('libopus.a','libvpx.a'):
        shutil.copy2(deps/'lib'/lib, gen/'deps/lib'/lib)
    translator = ios/'ShaderTranslator'
    manifest = json.loads((translator/'artifacts.json').read_text())
    for rel, expected in manifest['files_sha256'].items():
        if hashlib.sha256((translator/rel).read_bytes()).hexdigest() != expected:
            raise RuntimeError('embedded translator artifact changed: ' + rel)
    # the app project
    project = (ios/'project.yml').read_text()
    (ios/'project.generated.yml').write_text(project)
    xcodegen = shutil.which('xcodegen')
    run([xcodegen, 'generate', '--spec', ios/'project.generated.yml', '--project', ios], HERE, logs/'xcodegen.log', env)
    # Target-based build: the scheme/destination path fails on this Xcode ("iOS 26.5 is not installed"),
    # while the plain target build resolves the platform fine.
    run(['xcodebuild', '-project', ios/'GTAIOS.xcodeproj', '-target', 'GTAIOS', '-sdk', 'iphoneos',
         '-configuration', 'Release', 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO',
         'CODE_SIGN_IDENTITY=', 'build'],
        ios, logs/'xcodebuild.log', env)
    app = ios/'build/Release-iphoneos/GTA.app'
    if not app.is_dir():
        raise RuntimeError('xcodebuild produced no GTA.app; see ' + str(logs/'xcodebuild.log'))
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(app, out)
    shutil.copytree(translator/'GtaShaderAot.framework', out/'Frameworks/GtaShaderAot.framework', dirs_exist_ok=True)
    print(f'built {out}')

if __name__ == '__main__':
    main()
