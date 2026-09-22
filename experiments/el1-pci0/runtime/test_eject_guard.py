import importlib.util
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import eject_guard as g

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('exported_classifier', HERE / 'mount_guard.py')
classifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(classifier)

HOST = ('1 0 0:32 /@ / rw shared:1 - btrfs /dev/root rw\n'
        '2 1 259:7 / /mnt/omarchy-usb4 rw,relatime shared:20 - ext4 /dev/nvme1n1p4 rw\n')


def service_namespace(target_options):
    """A systemd service namespace: a slave copy of / and of the drive mount."""
    return ('3 0 0:32 /@ / ro,relatime shared:31 master:1 - btrfs /dev/root rw\n'
            '4 3 259:7 / /mnt/omarchy-usb4 ' + target_options +
            ' shared:32 master:20 - ext4 /dev/nvme1n1p4 rw\n')


class Clock:
    def __init__(self): self.value = 0.0
    def now(self): return self.value
    def sleep(self, seconds): self.value += seconds


class Settle(unittest.TestCase):
    def run_scan(self, scan):
        clock, report = Clock(), Mock()
        value = g.settle(scan, {}, allow_inherited=True, clock=clock.now, sleep=clock.sleep,
                         report=report, describe=lambda c, p: c)
        return value, clock, report

    def test_clear_no_delay_no_receipt(self):
        value, clock, report = self.run_scan(Mock(return_value=[]))
        self.assertEqual((value, clock.value), ([], 0))
        report.assert_not_called()

    def test_hostnamed_style_thirty_second_copy_clears(self):
        # Observed: the refused Eject came about 28 s before the service's idle exit.
        conflict = [dict(pid=4863, namespace='mnt:[4026533607]', mount='/mnt/omarchy-usb4')]
        scan = Mock(side_effect=[conflict] * (int(28.2 / g.INTERVAL) + 1) + [[]])
        value, clock, report = self.run_scan(scan)
        self.assertEqual(value, [])
        self.assertLess(clock.value, g.WAIT_SECONDS)
        receipt = report.call_args.args[0]
        self.assertTrue(receipt['cleared'])
        self.assertFalse(receipt['unmount_attempted'] or receipt['forced'])

    def test_previous_ten_second_bound_could_not_clear_it(self):
        conflict = [dict(pid=1, namespace='mnt:[1]', mount='/mnt/omarchy-usb4')]
        with patch.object(g, 'WAIT_SECONDS', 10.0):
            value, _, _ = self.run_scan(Mock(side_effect=[conflict] * 200 + [[]]))
        self.assertEqual(value, conflict)

    def test_persistent_alias_and_inaccessible_never_admitted(self):
        for conflict in ([dict(pid=30, namespace='mnt:[99]', mount='/alias')],
                         [dict(pid=40, inaccessible='Permission denied')]):
            scan = Mock(return_value=conflict)
            value, clock, report = self.run_scan(scan)
            self.assertEqual(value, conflict)
            self.assertLessEqual(scan.call_count, g.MAX_SCANS)
            self.assertLessEqual(clock.value, g.WAIT_SECONDS + 0.001)
            self.assertFalse(report.call_args.args[0]['cleared'])

    def test_post_unmount_never_waits_or_adds_inheritance(self):
        scan, sleep, report = Mock(return_value=['conflict']), Mock(), Mock()
        self.assertEqual(g.settle(scan, {}, sleep=sleep, report=report), ['conflict'])
        scan.assert_called_once_with({}, proc=Path('/proc'), allow_inherited=False)
        sleep.assert_not_called()
        report.assert_not_called()

    def test_scan_error_and_evidence_error_fail_closed(self):
        with self.assertRaises(PermissionError):
            self.run_scan(Mock(side_effect=PermissionError('denied')))
        with self.assertRaises(OSError):
            g.settle(Mock(side_effect=[[{'pid': 1}], []]), {}, allow_inherited=True,
                     sleep=lambda n: None, describe=lambda c, p: c,
                     report=Mock(side_effect=OSError('evidence failed')))

    def test_new_conflict_is_not_dismissed_when_old_process_exits(self):
        a, b = [{'pid': 1}], [{'pid': 2}]
        value, _, report = self.run_scan(Mock(side_effect=[a] + [b] * g.MAX_SCANS))
        self.assertEqual(value, b)
        self.assertEqual(len(report.call_args.args[0]['observations']), 2)

    def test_evidence_is_metadata_only_and_tolerates_blank_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp)
            (proc / '1/ns').mkdir(parents=True)
            os.symlink('user:[1]', proc / '1/ns/user')
            (proc / '77/ns').mkdir(parents=True)
            os.symlink('mnt:[2]', proc / '77/ns/mnt')
            os.symlink('user:[1]', proc / '77/ns/user')
            (proc / '77/comm').write_text('synthetic-service\n')
            (proc / '77/cgroup').write_text('0::/system.slice/synthetic.service\n')
            (proc / '77/mountinfo').write_text('\n' + service_namespace('ro,relatime') + '\n')
            value = g.details([dict(pid=77, namespace='mnt:[2]', mount='/mnt/omarchy-usb4'),
                               dict(pid=78, namespace='mnt:[3]', mount='/x')], proc)
        self.assertEqual(value[0]['comm'], 'synthetic-service')
        self.assertTrue(value[0]['init_user_namespace'] and value[0]['namespace_stable'])
        self.assertEqual(len(value[0]['mountinfo']), 2)
        self.assertIn('observation_error', value[1])

    def test_install_once_and_retain_original(self):
        session = SimpleNamespace(foreign_mounts=Mock(return_value=[]), RUN=Path('/not-used'),
                                  MOUNT=Path('/mnt/omarchy-usb4'))
        original = session.foreign_mounts
        g.install(session)
        self.assertEqual(session.foreign_mounts({}), [])
        original.assert_called_once()
        with self.assertRaises(ValueError):
            g.install(session)


class ExportedClassifier(unittest.TestCase):
    """The unchanged classifier explains the refusal; the helper never relaxes it."""

    def admitted(self, other):
        return classifier.inherited_only(classifier.parse(HOST), classifier.parse(other),
                                         {'259:7'}, '/mnt/omarchy-usb4')

    def test_read_only_service_copy_is_a_conflict(self):
        # A ProtectSystem=strict namespace created after the mount remounts it read-only.
        self.assertFalse(self.admitted(service_namespace('ro,relatime')))
        # The same copy received by propagation keeps the host options and is inherited.
        self.assertTrue(self.admitted(service_namespace('rw,relatime')))

    def test_foreign_layout_still_rejected(self):
        other = ('3 0 0:32 /@ / rw - btrfs /dev/root rw\n'
                 '4 3 259:7 / /mnt/omarchy-usb4 rw,relatime shared:20 - ext4 /dev/nvme1n1p4 rw\n')
        self.assertFalse(self.admitted(other))


if __name__ == '__main__':
    unittest.main()
