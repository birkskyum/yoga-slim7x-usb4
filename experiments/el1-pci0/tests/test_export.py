# SPDX-License-Identifier: GPL-2.0-only
"""Publication integrity and supplied interrupt assignment; no hardware."""
import hashlib
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Export(unittest.TestCase):
    def test_manifest_files_and_unchanged_production_code(self):
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

    def test_qualcomm_assignment_exact(self):
        text = (ROOT / 'pcie0-interrupts.dtsi').read_text()
        spis = [int(v) for v in re.findall(r'GIC_SPI\s+(\d+)\s+IRQ_TYPE_LEVEL_HIGH', text)]
        self.assertEqual(spis, [655,657,664,668,348,349,351,702,167,703,708,714,716])
        self.assertIn('/delete-property/ msi-map;', text)
        self.assertIn('/delete-property/ msi-map-mask;', text)
        names = re.search(r'interrupt-names\s*=([^;]+);', text).group(1)
        self.assertEqual(re.findall(r'"([^"]+)"', names),
                         ['msi'+str(i) for i in range(8)] + ['global'])

    def test_source_not_a_deployment_payload(self):
        for path in ROOT.rglob('*'):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            self.assertNotIn(path.suffix, ('.ko', '.efi', '.bin', '.img'))
            text = path.read_text()
            self.assertNotIn('BEGIN ' + 'OPENSSH PRIVATE KEY', text)


if __name__ == '__main__':
    unittest.main()
