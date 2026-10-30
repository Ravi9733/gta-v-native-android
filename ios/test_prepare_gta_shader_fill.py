"""Offline contract tests for the GTA-only shader-fill helper."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
SPEC = importlib.util.spec_from_file_location('gta_fill', Path(__file__).with_name('prepare_gta_shader_fill.py'))
fill = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fill)

class ShaderFillTests(unittest.TestCase):
    def test_texture_depth_wildcards_and_strict_other_dependencies(self):
        self.assertTrue(fill.deps_match({'tex': {'0': '*'}}, {'tex': {'0': 'cube'}}))
        self.assertFalse(fill.deps_match({'tex': {'0': '*'}}, {'tex': {'0': 'cube_depth'}}))
        self.assertTrue(fill.deps_match({'tex': {'0': '*_depth'}}, {'tex': {'0': 'cube_depth'}}))
        self.assertFalse(fill.deps_match({'attr': {'1': 'uint'}}, {}))
        self.assertFalse(fill.deps_match({'fout': {'0': 'uint'}}, {'fout': {'0': 'float'}}))
        self.assertTrue(fill.deps_match({'attr': {'1': 'float'}, 'tex': {'0': '*'}}, {}))
    def test_requests_reject_mismatched_identity_and_invalid_hex(self):
        variant = 'a' * 64 + '-' + 'b' * 12
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            path = folder / (variant + '.json')
            rq = {'variant': variant, 'stage': 'ps', 'code_hex': 'abcd', 'tex': {}}
            path.write_text(json.dumps(rq))
            self.assertEqual(set(fill.load_requests(folder)[0]), {variant})
            for field, value in [('variant', '../bad'), ('code_hex', 'abc'), ('stage', 'other'), ('tex', [])]:
                bad = dict(rq, **{field: value})
                path.write_text(json.dumps(bad))
                with self.subTest(field=field), self.assertRaises(fill.PackError):
                    fill.load_requests(folder)

if __name__ == '__main__':
    unittest.main()
