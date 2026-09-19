#!/usr/bin/env python3
"""Synthetic tables only; no firmware or personal capture in the fixture."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('audit_iort', Path(__file__).resolve().parents[1] / 'tools/audit-iort.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def table():
    data = bytearray(48 + 3 * 56 + 88 + 24)
    data[:4] = b'IORT'
    struct.pack_into('<I', data, 4, len(data))
    struct.pack_into('<II', data, 36, 5, 48)
    for i, segment in enumerate((0, 4, 6)):
        off = 48 + i * 56
        struct.pack_into('<BHBIII', data, off, 2, 56, 0, 0, 1, 36)
        struct.pack_into('<I', data, off + 28, segment)
        struct.pack_into('<IIIII', data, off + 36, 0, 0xffff, segment << 16, 216, 0)
    struct.pack_into('<BHBIII', data, 216, 4, 88, 0, 0, 1, 68)
    struct.pack_into('<Q', data, 232, 0x15400000)
    struct.pack_into('<IIIII', data, 284, 0, 0x7ffff, 0x80000, 304, 0)
    struct.pack_into('<BHBIII', data, 304, 0, 24, 0, 0, 0, 0)
    struct.pack_into('<II', data, 320, 1, 0)
    data[9] = (-sum(data)) & 255
    return data


class IortTests(unittest.TestCase):
    def test_calibration_and_inclusive_range(self):
        nodes = audit.decode(table())
        self.assertEqual(len(audit.calibrate(nodes)), 9)
        self.assertEqual(audit.chain(nodes, 0, 0xffff), (0xffff, 0x8ffff))
        with self.assertRaises(ValueError):
            audit.chain(nodes, 0, 0x10000)

    def test_corruption(self):
        original = table()
        for length in range(len(original)):
            with self.assertRaises(ValueError):
                audit.decode(original[:length])
        for off, value in ((8, 1), (48, 1), (49, 255), (60, 255), (100, 1),
                           (96, 0), (232, 1), (307, 1), (320, 2)):
            data = table();data[off] = value;data[9] = 0;data[9] = (-sum(data)) & 255
            with self.subTest(offset=off), self.assertRaises(ValueError):
                audit.calibrate(audit.decode(data))

    def test_overlapping_mapping(self):
        nodes = audit.decode(table());nodes[48]['maps'] *= 2
        with self.assertRaises(ValueError):
            audit.chain(nodes, 0, 0x100)

    def test_dsl_raw_dump(self):
        data = bytes(table())
        text = b'Raw Table Data:\n' + b'\n'.join(
            f'    {off:04X}: {data[off:off+16].hex(" ")} // synthetic'.encode()
            for off in range(0, len(data), 16))
        self.assertEqual(audit.raw_table(text), data)
        with self.assertRaises(ValueError):
            audit.raw_table(text.replace(b'0000:', b'0001:'))


if __name__ == '__main__':
    unittest.main()
