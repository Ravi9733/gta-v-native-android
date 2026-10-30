"""Stream and runtime-contract checks; no builds or device connections."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('pack_seed_gta', Path(__file__).with_name('pack_seed_gta.py'))
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)

class SeedPackTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.seed, self.output = self.root / 'cache', self.root / 'output'
        self.seed.mkdir()
        self.shader = 'a' * 64
        self.variant = self.shader + '-' + 'b' * 12
        self.contents = {
            self.variant + '_b.metal': b'fragment float4 main0() { return 0; }\n',
            self.variant + '_b.json': json.dumps(dict(chain=pack.runtime_chain(), deps={})).encode(),
            self.variant + '_b.metallib': b'MTLB\0air64_v28-apple-ios26.5.0\0air.compile.fast_math_disable',
            self.shader + '.variants': json.dumps([dict(variant=self.variant, deps={})]).encode(),
            self.variant + '.code': b'translator input must not enter the runtime seed',
        }
        self.write_source()
    def write_source(self):
        for name, data in self.contents.items():
            (self.seed / name).write_bytes(data)
        manifest = dict(chain=pack.runtime_chain(), ios_metallibs=1, ios_metal_flags='-fmetal-math-mode=safe',
                        files={n: hashlib.sha256(v).hexdigest() for n, v in self.contents.items()})
        (self.seed / 'seed-manifest.json').write_text(json.dumps(manifest))
    def unpack(self, index):
        stream = io.BytesIO(b''.join((self.output / p['name']).read_bytes() for p in index['parts']))
        self.assertEqual(stream.read(9), b'TOTKPACK1')
        files = {}
        while True:
            length = struct.unpack('<H', stream.read(2))[0]
            if not length:
                break
            name = stream.read(length).decode()
            size = struct.unpack('<Q', stream.read(8))[0]
            files[name] = stream.read(size)
            self.assertEqual(len(files[name]), size)
        self.assertEqual(stream.read(), b'')
        return files
    def test_roundtrip_across_parts_runtime_hashes_marker_and_source_unchanged(self):
        before = {p.name: p.read_bytes() for p in self.seed.iterdir()}
        index = pack.build_pack(self.seed, self.output, part_size=37)
        self.assertGreater(len(index['parts']), 2)
        for number, part in enumerate(index['parts']):
            raw = (self.output / part['name']).read_bytes()
            self.assertTrue(part['name'].endswith(f'.{number:03d}'))
            self.assertEqual(part['bytes'], len(raw))
            self.assertEqual(part['sha256'], hashlib.sha256(raw).hexdigest())
            self.assertLessEqual(len(raw), 37)
        files = self.unpack(index)
        self.assertNotIn(self.variant + '.code', files)
        manifest = json.loads(files.pop('seed-manifest.json'))
        self.assertEqual(manifest['schema'], 1)
        self.assertEqual(manifest['chain'], pack.runtime_chain())
        self.assertEqual(manifest['shaders'], 1)
        self.assertEqual(set(files), set(manifest['files']))
        for name, raw in files.items():
            self.assertRegex(name, pack.SAFE_NAME)
            self.assertEqual(hashlib.sha256(raw).hexdigest(), manifest['files'][name])
        self.assertEqual((self.output / 'shader-seed.txt').read_text().splitlines(), [index['folder'], index['fingerprint']])
        self.assertEqual(len(index['fingerprint']), 64)
        self.assertEqual(index['fingerprint'], hashlib.sha256((self.output / 'seed-manifest.json').read_bytes()).hexdigest())
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.seed.iterdir()})
    def test_hash_mismatch_keeps_old_marker_and_packs(self):
        self.output.mkdir()
        (self.output / 'shader-seed.txt').write_text('old seed remains\n')
        (self.output / 'old.pack.000').write_bytes(b'keep')
        (self.seed / (self.variant + '_b.metal')).write_bytes(b'corrupt')
        with self.assertRaisesRegex(pack.PackError, 'SHA-256 mismatch'):
            pack.build_pack(self.seed, self.output, part_size=37)
        self.assertEqual((self.output / 'shader-seed.txt').read_text(), 'old seed remains\n')
        self.assertEqual(sorted(p.name for p in self.output.iterdir()), ['old.pack.000', 'shader-seed.txt'])
    def test_preserves_invariance_compiler_attestation(self):
        path = self.seed / 'seed-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['ios_metal_flags'] = '-fmetal-math-mode=safe -fpreserve-invariance'
        path.write_text(json.dumps(manifest))
        packed = pack.prepare(self.seed)[0]
        self.assertEqual(packed['ios_metal_flags'], manifest['ios_metal_flags'])
    def test_rejects_false_or_unsupported_compiler_attestation(self):
        path = self.seed / 'seed-manifest.json'
        manifest = json.loads(path.read_text())
        for flags in ['-fmetal-math-mode=safely', '-fmetal-math-mode=safe -ffast-math',
                      '-fmetal-math-mode=safe -fpreserve-invariance -fpreserve-invariance', None]:
            with self.subTest(flags=flags):
                manifest['ios_metal_flags'] = flags
                path.write_text(json.dumps(manifest))
                with self.assertRaises(pack.PackError):
                    pack.prepare(self.seed)
    def test_dynamic_index_refresh_uses_verified_reflection_and_reports_hashes(self):
        name = self.shader + '.variants'
        old_sha = hashlib.sha256(self.contents[name]).hexdigest()
        current = json.dumps([dict(variant=self.variant, deps={})] * 3).encode()
        (self.seed / name).write_bytes(current)  # Manifest predates Mac runtime updates.
        index = pack.build_pack(self.seed, self.output, part_size=97)
        changed = index['report']['updated_dynamic_indexes']
        self.assertEqual(len(changed), 1)
        self.assertEqual(changed[0]['name'], name)
        self.assertEqual(changed[0]['old_sha256'], old_sha)
        self.assertEqual(changed[0]['new_sha256'], hashlib.sha256(current).hexdigest())
        files = self.unpack(index)
        self.assertEqual(len(json.loads(files[name])), 1)
        self.assertEqual(changed[0]['normalized_sha256'], hashlib.sha256(files[name]).hexdigest())
        self.assertEqual((self.seed / name).read_bytes(), current)
    def test_dynamic_index_refresh_rejects_untrusted_dependencies(self):
        name = self.shader + '.variants'
        (self.seed / name).write_text(json.dumps([dict(variant=self.variant, deps={'tex': {'0': 'int'}})]))
        with self.assertRaisesRegex(pack.PackError, 'dependencies disagree with verified reflection'):
            pack.build_pack(self.seed, self.output)
        self.assertFalse(self.output.exists())
    def test_incomplete_variant_is_explicitly_reported_and_removed(self):
        missing = self.shader + '-' + 'c' * 12
        name = self.shader + '.variants'
        self.contents[name] = json.dumps([dict(variant=self.variant, deps={}), dict(variant=missing, deps={})]).encode()
        self.write_source()
        index = pack.build_pack(self.seed, self.output, part_size=53)
        rows = json.loads(self.unpack(index)[name])
        self.assertEqual([row['variant'] for row in rows], [self.variant])
        rejected = index['report']['skipped_incomplete_variants'][0]
        self.assertEqual(rejected['variant'], missing)
        self.assertEqual(len(rejected['missing']), 3)
    def test_no_complete_variant_fails_without_publishing(self):
        del self.contents[self.variant + '_b.metallib']
        self.write_source()
        with self.assertRaisesRegex(pack.PackError, 'no complete iOS shader variants'):
            pack.build_pack(self.seed, self.output)
        self.assertFalse(self.output.exists())
    def test_macos_and_simulator_libraries_are_rejected(self):
        for target in [b'air64_v28-apple-macos26.5.0', b'air64_v28-apple-ios26.5.0-simulator']:
            with self.subTest(target=target):
                self.contents[self.variant + '_b.metallib'] = b'MTLB\0' + target
                self.write_source()
                with self.assertRaisesRegex(pack.PackError, 'not an iPhone-device metallib'):
                    pack.build_pack(self.seed, self.output)
                self.assertFalse(self.output.exists())
    def test_publication_error_restores_previous_metadata_and_new_parts(self):
        self.output.mkdir()
        (self.output / 'shader-seed.txt').write_text('previous marker\n')
        (self.output / 'seed-manifest.json').write_bytes(b'previous manifest')
        original_replace = pack.os.replace
        def fail_once(source, target):
            if Path(source).name == 'pack-index.json':
                raise OSError('simulated publication failure')
            return original_replace(source, target)
        with mock.patch.object(pack.os, 'replace', side_effect=fail_once):
            with self.assertRaisesRegex(OSError, 'simulated publication failure'):
                pack.build_pack(self.seed, self.output, part_size=127)
        self.assertEqual((self.output / 'shader-seed.txt').read_text(), 'previous marker\n')
        self.assertEqual((self.output / 'seed-manifest.json').read_bytes(), b'previous manifest')
        self.assertEqual(sorted(p.name for p in self.output.iterdir()), ['seed-manifest.json', 'shader-seed.txt'])

if __name__ == '__main__':
    unittest.main()
