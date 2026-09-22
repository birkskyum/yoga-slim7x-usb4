import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

import eject_guard as g


class Clock:
    def __init__(self): self.value = 0
    def now(self): return self.value
    def sleep(self, seconds): self.value += seconds


class Settle(unittest.TestCase):
    def run_scan(self, scan, **kwargs):
        clock = Clock()
        report = Mock()
        value = g.settle(scan, {}, allow_inherited=True, clock=clock.now,
                         sleep=clock.sleep, report=report, describe=lambda c,p:c, **kwargs)
        return value, clock, report

    def test_clear_no_delay_no_receipt(self):
        scan = Mock(return_value=[])
        result, clock, report = self.run_scan(scan)
        self.assertEqual(result, [])
        self.assertEqual(scan.call_count, 1)
        self.assertEqual(clock.value, 0)
        report.assert_not_called()

    def test_transient_namespace_clears_without_waiver(self):
        conflict = [dict(pid=42, namespace='mnt:[100]', mount='/mnt/omarchy-usb4')]
        scan = Mock(side_effect=[conflict]*25 + [[]])
        value, clock, report = self.run_scan(scan)
        self.assertEqual(value, [])
        self.assertEqual(scan.call_count, 26)
        self.assertLess(clock.value, g.WAIT_SECONDS)
        receipt = report.call_args.args[0]
        self.assertTrue(receipt['cleared'])
        self.assertFalse(receipt['unmount_attempted'])
        self.assertEqual(receipt['observations'][0]['conflicts'], conflict)

    def test_persistent_alias_and_inaccessible_never_admitted(self):
        for conflict in ([dict(pid=30, namespace='mnt:[99]', mount='/alias')],
                         [dict(pid=40, inaccessible='Permission denied')]):
            scan = Mock(return_value=conflict)
            value, clock, report = self.run_scan(scan)
            self.assertEqual(value, conflict)
            self.assertLessEqual(scan.call_count, g.MAX_SCANS)
            self.assertLessEqual(clock.value, g.WAIT_SECONDS+0.001)
            self.assertFalse(report.call_args.args[0]['cleared'])

    def test_post_unmount_never_waits_or_adds_inheritance(self):
        scan, sleep, report = Mock(return_value=['conflict']), Mock(), Mock()
        self.assertEqual(g.settle(scan, {}, sleep=sleep, report=report), ['conflict'])
        scan.assert_called_once_with({}, proc=Path('/proc'), allow_inherited=False)
        sleep.assert_not_called(); report.assert_not_called()

    def test_scan_error_and_evidence_error_fail_closed(self):
        with self.assertRaises(PermissionError):
            self.run_scan(Mock(side_effect=PermissionError('denied')))
        with self.assertRaises(OSError):
            g.settle(Mock(side_effect=[[{'pid':1}], []]), {}, allow_inherited=True,
                     sleep=lambda n:None, describe=lambda c,p:c,
                     report=Mock(side_effect=OSError('evidence failed')))

    def test_new_conflict_is_not_dismissed_when_old_process_exits(self):
        a, b = [{'pid':1}], [{'pid':2}]
        scan = Mock(side_effect=[a]+[b]*g.MAX_SCANS)
        value, clock, report = self.run_scan(scan)
        self.assertEqual(value, b)
        self.assertEqual(len(report.call_args.args[0]['observations']), 2)

    def test_install_once_and_retain_original_no_io_functions(self):
        session = SimpleNamespace(foreign_mounts=Mock(return_value=[]), RUN=Path('/not-used'))
        original = session.foreign_mounts
        g.install(session)
        self.assertEqual(session.foreign_mounts({}), [])
        original.assert_called_once()
        with self.assertRaises(ValueError): g.install(session)

    def test_production_classifier_still_rejects_foreign_layouts(self):
        source = Path(__file__).resolve().parent/'mount_guard.py'
        spec = importlib.util.spec_from_file_location('frozen_guard', source.resolve())
        guard = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard)
        host = guard.parse('1 0 0:32 /@ / rw shared:1 - btrfs /dev/root rw\n'
                           '2 1 259:7 / /mnt/omarchy-usb4 rw shared:20 - ext4 /dev/nvme1n1p4 rw\n')
        other = guard.parse('3 0 0:32 /@ / rw - btrfs /dev/root rw\n'
                            '4 3 259:7 / /mnt/omarchy-usb4 rw shared:20 - ext4 /dev/nvme1n1p4 rw\n')
        scan = Mock(side_effect=lambda *a,**kw: [] if guard.inherited_only(
            host, other, {'259:7'}, '/mnt/omarchy-usb4') else [{'pid':10}])
        self.assertEqual(self.run_scan(scan)[0], [{'pid':10}])


if __name__ == '__main__': unittest.main()
