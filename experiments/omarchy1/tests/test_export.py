# SPDX-License-Identifier: GPL-2.0-only
"""Publication integrity for the omarchy1 reference export; no hardware."""
import hashlib
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = re.compile(r'\b(?:\d{1,3}\.){3}\d{1,3}\b|'
                     r'\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b|'
                     r'\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b|' + 'PART' + 'UUID|' + 'crypt' + 'device')


class Export(unittest.TestCase):
    def test_manifest_files_and_hashes(self):
        manifest = json.loads((ROOT / 'export.json').read_text())
        paths = [item['export'] for item in manifest['files']]
        self.assertEqual(len(paths), len(set(paths)))
        for item in manifest['files']:
            relative = Path(item['export'])
            self.assertFalse(relative.is_absolute())
            self.assertNotIn('..', relative.parts)
            raw = (ROOT / relative).read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), item['public_sha256'])
            if not item['changes']:
                self.assertEqual(item['source_sha256'], item['public_sha256'])

    def test_no_deployment_payload_or_machine_identity(self):
        for path in ROOT.rglob('*'):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            self.assertNotIn(path.suffix, ('.ko', '.efi', '.bin', '.img'))
            text = path.read_text()
            self.assertNotIn('BEGIN ' + 'OPENSSH PRIVATE KEY', text)
            self.assertIsNone(PRIVATE.search(text), str(path))


if __name__ == '__main__':
    unittest.main()
