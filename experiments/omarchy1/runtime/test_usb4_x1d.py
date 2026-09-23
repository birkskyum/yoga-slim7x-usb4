#!/usr/bin/env python3
"""Offline tests for usb4_x1d against a simulated x1 router. No hardware."""
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import usb4_x1d as d  # noqa: E402


class FakeKernel:
    """Models the managed x1_general stages as the host driver implements them."""

    def __init__(self):
        self.generation = 1
        self.orientation = 0
        self.prepared = self.waiting = self.complete = self.retired = False
        self.initial_event = self.events = self.disconnect_event = 0
        self.rearm = False
        self.negotiation_failures = 0
        self.negotiate = True  # Whether the next attach enters USB4.
        self.removed = False
        self.stages = []
        self.stores = []
        self.mounts = []
        self.published = []
        self.notified = []
        self.disk_name = None
        self.error = 0
        self.held_elsewhere = False
        self.idle = False  # Retired by the idle power-down, not by an eject.
        self.idle_refusals = 0  # Refusals that leave the session unchanged.
        self.idle_fail = False  # A terminal fault after the freeze.
        self.idle_attempted = False
        self.command_pending = False  # The router already sent its connect command.
        self.unmount_busy = False
        self.unmounted = []
        self.pulled_retired = False
        self.pulled_fail = False
        self.desktop_busy = False  # Files or a shell keeps the mount even for the desktop.
        self.desktop_unmounted = []

    # Physical actions -------------------------------------------------
    def plug(self):
        self.events += 1
        self.orientation = 1

    def unplug(self):
        self.events += 1
        self.disconnect_event = self.events
        self.orientation = 0
        if self.rearm:
            self.rearm = False

    # Kernel interface -------------------------------------------------
    def read(self, name):
        if name == 'session_state':
            return dict(busy='0', generation=str(self.generation), managed='1',
                        pristine='0' if self.prepared else '1', orientation=str(self.orientation),
                        prepared=str(int(self.prepared)), waiting=str(int(self.waiting)),
                        complete=str(int(self.complete)), fully_retired=str(int(self.retired)),
                        platform_retired=str(int(self.retired)), platform_error='0',
                        initial_event=str(self.initial_event),
                        disconnect_event=str(self.disconnect_event), cached='1')
        if name == 'inventory_state':
            return dict(busy='0', error=str(self.error), terminal='0',
                        step='connect-once' if self.waiting else 'nvme-bound',
                        negotiation_rearm=str(int(self.rearm)),
                        negotiation_failures=str(self.negotiation_failures))
        if name == 'general_state':
            return dict(busy='0', endpoint_removed=str(int(self.removed)), endpoint_remove_error='0',
                        idle_retire_attempted=str(int(self.idle_attempted)),
                        idle_retired=str(int(self.idle)), idle_retire_error='-5' if self.idle_fail else '0',
                        idle_retire_stage='power-stop' if self.idle_fail else 'none',
                        pulled_retired=str(int(self.pulled_retired)), pulled_retire_error='0')
        stage = name.replace('_quiesce_state', '')
        done = stage in self.stages
        return dict(busy='0', attempted=str(int(done)), complete=str(int(done)), error='0')

    def store(self, name, token):
        self.stores.append((name, token))
        if name == 'inventory_once':
            assert token == b'inventory any\n'
            if not self.prepared:
                assert self.orientation == 0
                self.prepared = self.waiting = True
                self.initial_event = self.events
                return
            assert self.waiting and self.orientation and not self.rearm
            if not self.negotiate:
                self.negotiation_failures += 1
                self.rearm = True
                raise OSError(errno.ENOTCONN, 'not negotiated')
            self.waiting = False
            self.complete = True
            self.disk_name = 'nvme1n1'
        elif name == 'endpoint_remove_once':
            assert token == b'remove-endpoint-v1\n' and not self.mounts and self.complete
            self.removed = True
            self.disk_name = None
        elif name.endswith('_quiesce_once'):
            stage = name.replace('_quiesce_once', '')
            order = ['tunnel', 'host', 'power']
            assert self.removed and self.stages == order[:order.index(stage)]
            self.stages.append(stage)
            if stage == 'power':
                self.retired = True
        elif name == 'idle_retire_once':
            assert token == b'retire-idle-v1\n' and not self.idle_attempted
            if self.idle_refusals:
                self.idle_refusals -= 1
                raise OSError(errno.EBUSY, 'refused, session kept')
            if not (self.prepared and self.waiting) or self.complete or self.command_pending:
                raise OSError(errno.EPERM, 'not idle')
            self.idle_attempted = True
            if self.idle_fail:
                raise OSError(errno.EIO, 'power stop failed')
            self.idle = self.retired = True
        elif name == 'pulled_retire_once':
            assert token == b'retire-pulled-v1\n' and self.complete and not self.removed
            assert self.orientation == 0 and self.disconnect_event > self.initial_event
            if self.pulled_fail:
                raise OSError(errno.EIO, 'pulled retirement failed')
            self.pulled_retired = self.retired = True
            self.disk_name = None
            self.mounts = []  # udisks cleans the dead mount up on its own.
        elif name == 'renew_session':
            assert token == (str(self.generation) + '\n').encode() and self.retired
            if self.orientation or (not self.idle and self.disconnect_event <= self.initial_event):
                raise OSError(errno.EBUSY, 'attached')
            self.generation += 1
            self.idle = self.idle_attempted = self.pulled_retired = False
            self.prepared = self.waiting = self.complete = self.retired = self.removed = False
            self.stages = []
            self.negotiation_failures = 0
        else:
            raise AssertionError(name)

    def parameters_ok(self):
        pass

    def install_rule(self):
        pass

    def disk(self):
        return self.disk_name

    def mounted(self, disk):
        return list(self.mounts)

    def released(self, disk):
        return not self.mounts and not self.held_elsewhere

    def mounted_devices(self, disk):
        return ['nvme1n1p4'] if self.mounts else []

    def unmount(self, device):
        if self.unmount_busy:
            return False
        self.unmounted.append(device)
        self.mounts = []
        return True

    def unmount_desktop(self, point):
        self.desktop_unmounted.append(point)
        if self.desktop_busy:
            return False
        self.mounts = [m for m in self.mounts if m != point]
        return True

    def notify_text(self, text):
        self.notified.append(text)

    def settle(self):
        pass

    def notify(self, phase):
        self.notified.append(phase)

    def publish(self, value):
        self.published.append(value['phase'])


def make():
    kernel = FakeKernel()
    service = d.Service(d.Router(kernel, sleep=lambda s: None), kernel)
    service.start()
    return kernel, service


class Lifecycle(unittest.TestCase):
    def test_full_cycle_eject_after_unmount_and_reconnect(self):
        kernel, service = make()
        self.assertTrue(service.step())
        self.assertEqual((service.phase, service.generation), ('waiting', 1))
        kernel.plug()
        service.step()
        self.assertEqual((service.phase, service.disk), ('connected', 'nvme1n1'))
        service.step()
        self.assertEqual(service.phase, 'connected')  # Nothing mounted yet: no eject.
        kernel.mounts = ['/run/media/birk/LACIE']
        service.step()
        kernel.mounts = []
        service.step()
        self.assertEqual(service.phase, 'safe-to-unplug')
        self.assertEqual([n for n, _ in kernel.stores],
                         ['inventory_once', 'inventory_once', 'endpoint_remove_once',
                          'tunnel_quiesce_once', 'host_quiesce_once', 'power_quiesce_once'])
        kernel.unplug()
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))
        self.assertEqual(kernel.stores[-2:], [('renew_session', b'1\n'), ('inventory_once', b'inventory any\n')])
        self.assertEqual(kernel.notified, ['connected', 'ejecting', 'safe-to-unplug'])

    def test_drive_attached_at_start_waits_for_unplug(self):
        kernel, service = make()
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'unplug-to-start')
        self.assertEqual(kernel.stores, [])
        kernel.unplug()
        service.step()
        self.assertEqual(service.phase, 'waiting')
        self.assertEqual(kernel.notified, ['unplug-to-start', 'waiting'])

    def test_failed_negotiation_asks_for_replug_then_connects(self):
        kernel, service = make()
        service.step()
        kernel.negotiate = False
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'replug-required')
        service.step()
        self.assertEqual(service.phase, 'replug-required')  # Still plugged: abandoned.
        kernel.unplug()
        kernel.negotiate = True
        service.step()
        self.assertEqual(service.phase, 'waiting')
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'connected')

    def test_pull_while_connected_recovers_without_restart(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.mounts = ['/run/media/birk/LACIE']
        service.step()
        kernel.unplug()
        self.assertTrue(service.step())
        self.assertEqual((service.phase, service.disk), ('pulled', None))
        stores = [n for n, _ in kernel.stores]
        self.assertEqual(stores[-1], 'pulled_retire_once')
        self.assertNotIn('endpoint_remove_once', stores)  # Nothing reaches the gone drive.
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))
        self.assertIn('pulled', kernel.notified)
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'connected')

    def test_pull_while_releasing_recovers(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.mounts = ['/x']
        service.step()
        kernel.mounts = []
        kernel.held_elsewhere = True
        service.step()
        self.assertEqual(service.phase, 'releasing')
        kernel.unplug()
        service.step()
        self.assertEqual(service.phase, 'pulled')
        service.step()
        self.assertEqual(service.phase, 'waiting')

    def test_failed_pulled_retirement_fails_closed(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.pulled_fail = True
        kernel.unplug()
        self.assertFalse(service.step())
        self.assertEqual(service.phase, 'failed')

    def test_replug_before_renewal_waits_for_unplug(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.unplug()
        service.step()
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'replug-after-pull')
        kernel.unplug()
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))

    def test_manual_eject_needs_unmounted_drive(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.mounts = ['/run/media/birk/LACIE']
        with self.assertRaises(d.Stop):
            service.eject_now()
        kernel.mounts = []
        service.eject_now()
        self.assertEqual(service.phase, 'safe-to-unplug')

    def test_kernel_refusal_stops_without_further_stores(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.mounts = ['/x']
        service.step()
        kernel.mounts = []
        original = kernel.store

        def refuse(name, token):
            if name == 'tunnel_quiesce_once':
                raise OSError(errno.EPERM, 'refused')
            original(name, token)
        kernel.store = refuse
        self.assertFalse(service.step())
        self.assertEqual(service.phase, 'failed')
        self.assertNotIn('host_quiesce_once', [n for n, _ in kernel.stores])


class Restart(unittest.TestCase):
    def restarted(self, kernel):
        service = d.Service(d.Router(kernel, sleep=lambda s: None), kernel)
        service.start()
        return service

    def test_idle_prepared_router_is_adopted(self):
        kernel, service = make()
        service.step()
        service = self.restarted(kernel)
        self.assertTrue(service.step())
        self.assertEqual((service.phase, service.generation), ('waiting', 1))
        self.assertEqual(names(kernel), ['inventory_once'])  # Nothing new reached the router.
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'connected')

    def test_adopted_router_powers_down_for_sleep_and_renews(self):
        kernel, service = make()
        service.step()
        service = self.restarted(kernel)
        service.step()
        self.assertTrue(service.sleep_request('pre')[0])
        service.sleep_request('post')
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))

    def test_attached_or_rearmed_router_is_not_adopted(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service = self.restarted(kernel)
        service.step()
        self.assertEqual((service.phase, service.generation), ('unplug-to-start', 0))
        kernel.rearm = True
        kernel.unplug()
        kernel.rearm = True  # A negotiation that was re-armed before the restart.
        self.assertFalse(service.step())
        self.assertEqual(service.phase, 'failed')  # As before: a restart is needed.

    def test_retired_router_is_renewed_once_the_port_is_empty(self):
        for how in ('eject', 'pull', 'sleep'):
            kernel, service = connected() if how != 'sleep' else make()
            if how == 'eject':
                kernel.mounts = []
                service.step()
                self.assertEqual(service.phase, 'safe-to-unplug')
            elif how == 'pull':
                kernel.unplug()
                service.step()
                self.assertEqual(service.phase, 'pulled')
            else:
                service.step()
                service.sleep_request('pre')
            service = self.restarted(kernel)
            service.step()
            if how == 'eject':
                self.assertEqual(service.phase, 'unplug-to-start')  # The drive is still attached.
                kernel.unplug()
                service.step()
            self.assertEqual((how, service.phase, service.generation), (how, 'waiting', 2))
            self.assertEqual(kernel.stores[-2:], [('renew_session', b'1\n'), ('inventory_once', b'inventory any\n')])

    def test_half_stopped_router_is_not_adopted(self):
        kernel, service = connected()
        kernel.mounts = []
        original = kernel.store

        def refuse(name, token):
            if name == 'host_quiesce_once':
                raise OSError(errno.EIO, 'failed')
            original(name, token)
        kernel.store = refuse
        self.assertFalse(service.step())
        kernel.store = original
        kernel.unplug()
        service = self.restarted(kernel)
        self.assertIsNone(service.router.adopt())
        self.assertFalse(service.step())
        self.assertEqual(service.phase, 'failed')
        self.assertNotIn('renew_session', names(kernel))

    def test_fresh_boot_still_prepares(self):
        kernel, service = make()
        self.assertIsNone(service.router.adopt())
        service.step()
        self.assertEqual(names(kernel), ['inventory_once'])


class Release(unittest.TestCase):
    def test_copy_in_another_namespace_delays_eject(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.mounts = ['/run/media/birk/LACIE']
        service.step()
        kernel.mounts = []
        kernel.held_elsewhere = True
        service.step()
        service.step()
        self.assertEqual(service.phase, 'releasing')
        self.assertNotIn('endpoint_remove_once', [n for n, _ in kernel.stores])
        kernel.held_elsewhere = False
        service.step()
        self.assertEqual(service.phase, 'safe-to-unplug')
        self.assertEqual(kernel.notified, ['connected', 'ejecting', 'safe-to-unplug'])

    def test_remount_while_releasing_returns_to_connected(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.mounts = ['/x']
        service.step()
        kernel.mounts = []
        kernel.held_elsewhere = True
        service.step()
        kernel.mounts = ['/x']
        service.step()
        self.assertEqual(service.phase, 'connected')

    def test_manual_eject_refused_while_held_elsewhere(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.held_elsewhere = True
        with self.assertRaises(d.Stop):
            service.eject_now()
        self.assertEqual(service.phase, 'connected')


def connected(mounted=True):
    kernel, service = make()
    service.step()
    kernel.plug()
    service.step()
    if mounted:
        kernel.mounts = ['/run/media/birk/LACIE']
        service.step()
    return kernel, service


def names(kernel):
    return [n for n, _ in kernel.stores]


class Sleep(unittest.TestCase):
    def test_idle_router_powers_down_and_returns_after_resume(self):
        kernel, service = make()
        service.step()
        self.assertEqual(service.sleep_request('pre'), (True, d.MESSAGES['asleep']))
        self.assertEqual((service.phase, service.sleeping), ('asleep', True))
        self.assertEqual(names(kernel), ['inventory_once', 'idle_retire_once'])
        service.step()
        service.step()
        self.assertEqual(names(kernel), ['inventory_once', 'idle_retire_once'])  # Held until resume.
        self.assertEqual(service.sleep_request('post')[0], True)
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))
        self.assertEqual(kernel.stores[-2:], [('renew_session', b'1\n'), ('inventory_once', b'inventory any\n')])
        self.assertEqual(kernel.notified, [])

    def test_mounted_drive_is_unmounted_and_ejected_then_needs_a_replug(self):
        kernel, service = connected()
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(kernel.unmounted, ['nvme1n1p4'])
        self.assertEqual(service.phase, 'ejected-for-sleep')
        self.assertEqual(names(kernel)[-4:], ['endpoint_remove_once', 'tunnel_quiesce_once',
                                              'host_quiesce_once', 'power_quiesce_once'])
        service.sleep_request('post')
        service.step()
        self.assertEqual(service.phase, 'ejected-for-sleep')  # Still attached.
        kernel.unplug()
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))
        self.assertIn('ejected-for-sleep', kernel.notified)

    def test_open_files_keep_the_drive_and_refuse_sleep(self):
        kernel, service = connected()
        kernel.unmount_busy = True
        ok, message = service.sleep_request('pre')
        self.assertEqual((ok, message), (False, d.BUSY_FOR_SLEEP))
        self.assertEqual((service.phase, service.sleeping), ('connected', False))
        self.assertNotIn('endpoint_remove_once', names(kernel))
        self.assertIn(d.BUSY_FOR_SLEEP, kernel.notified)
        self.assertTrue(service.step())

    def test_copy_held_elsewhere_past_the_budget_refuses_sleep(self):
        kernel, service = connected()
        now = [0.0]
        service.router.clock = lambda: now[0]

        def later(seconds):
            now[0] += 5
        service.router.sleep = later
        kernel.held_elsewhere = True
        self.assertEqual(service.sleep_request('pre'), (False, d.BUSY_FOR_SLEEP))
        self.assertNotIn('endpoint_remove_once', names(kernel))
        kernel.held_elsewhere = False
        service.step()
        self.assertEqual(service.phase, 'safe-to-unplug')  # Unmounted for sleep, so ejected.

    def test_never_mounted_drive_is_ejected_for_sleep(self):
        kernel, service = connected(mounted=False)
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual((service.phase, kernel.unmounted), ('ejected-for-sleep', []))

    def test_untouched_attachment_powers_down_and_waits_for_unplug(self):
        kernel, service = make()
        service.step()
        kernel.negotiate = False
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'replug-required')
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(service.phase, 'asleep')
        service.sleep_request('post')
        service.step()
        self.assertEqual(service.phase, 'unplug-to-start')
        kernel.unplug()
        kernel.negotiate = True
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))

    def test_connecting_drive_finishes_then_is_ejected(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        kernel.command_pending = True
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(service.phase, 'ejected-for-sleep')
        self.assertIn('inventory_once', names(kernel)[1:])

    def test_refusal_retries_unchanged(self):
        kernel, service = make()
        service.step()
        kernel.idle_refusals = 2
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(names(kernel).count('idle_retire_once'), 3)
        self.assertEqual(service.phase, 'asleep')

    def test_fault_after_freeze_fails_closed(self):
        kernel, service = make()
        service.step()
        kernel.idle_fail = True
        ok, message = service.sleep_request('pre')
        self.assertFalse(ok)
        self.assertIn('power-stop', message)
        self.assertEqual(service.phase, 'failed')
        self.assertFalse(service.step())

    def test_already_retired_or_unstarted_needs_nothing(self):
        kernel, service = connected()
        kernel.mounts = []
        service.step()
        self.assertEqual(service.phase, 'safe-to-unplug')
        before = list(kernel.stores)
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(kernel.stores, before)
        kernel, service = make()
        kernel.plug()
        service.step()
        self.assertEqual(service.phase, 'unplug-to-start')
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(kernel.stores, [])

    def test_missing_post_hook_resumes_after_grace(self):
        kernel, service = make()
        now = [0.0]
        service.router.clock = lambda: now[0]
        service.step()
        service.sleep_request('pre')
        now[0] += d.SLEEP_GRACE - 1
        service.step()
        self.assertEqual(service.phase, 'asleep')
        now[0] += 2
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))

    def test_cable_arriving_during_renewal_waits(self):
        kernel, service = make()
        service.step()
        service.sleep_request('pre')
        service.sleep_request('post')
        original = kernel.store

        def plug_first(name, token):
            if name == 'renew_session':
                kernel.plug()
            original(name, token)
        kernel.store = plug_first
        self.assertTrue(service.step())
        self.assertEqual(service.phase, 'unplug-to-start')
        kernel.store = original
        kernel.unplug()
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))

    def test_sleep_after_a_pull_needs_nothing(self):
        kernel, service = make()
        service.step()
        kernel.plug()
        service.step()
        kernel.unplug()
        service.step()
        before = list(kernel.stores)
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(kernel.stores, before)

    def test_unknown_action_changes_nothing(self):
        kernel, service = make()
        service.step()
        self.assertEqual(service.sleep_request('hibernate'), (False, 'Unknown sleep action'))
        self.assertEqual(service.phase, 'waiting')


class FakeLock:
    def __init__(self):
        self.events = []

    def take(self):
        self.events.append('take')

    def release(self):
        self.events.append('release')


class Announced(unittest.TestCase):
    """logind's PrepareForSleep arrives while the desktop still runs; the hook comes later."""

    def test_desktop_unmounts_first_then_the_hook_finds_nothing_to_do(self):
        kernel, service = connected()
        lock = FakeLock()
        d.answer_logind(service, lock, [True])
        self.assertEqual(kernel.desktop_unmounted, ['/run/media/birk/LACIE'])
        self.assertEqual(kernel.unmounted, [])  # udisks was not needed.
        self.assertEqual((service.phase, service.sleeping), ('ejected-for-sleep', True))
        self.assertEqual(lock.events, ['release'])
        before = list(kernel.stores)
        self.assertEqual(service.sleep_request('pre'), (True, d.MESSAGES['ejected-for-sleep']))
        self.assertEqual(kernel.stores, before)
        service.sleep_request('post')
        d.answer_logind(service, lock, [False])
        self.assertEqual(lock.events, ['release', 'take'])
        kernel.unplug()
        service.step()
        self.assertEqual((service.phase, service.generation), ('waiting', 2))

    def test_desktop_failure_falls_back_to_udisks(self):
        kernel, service = connected()
        kernel.desktop_busy = True
        d.answer_logind(service, FakeLock(), [True])
        self.assertEqual((kernel.desktop_unmounted, kernel.unmounted), (['/run/media/birk/LACIE'], ['nvme1n1p4']))
        self.assertEqual(service.phase, 'ejected-for-sleep')

    def test_refusal_is_shown_once_and_answers_the_hook(self):
        kernel, service = connected()
        kernel.desktop_busy = kernel.unmount_busy = True
        lock = FakeLock()
        d.answer_logind(service, lock, [True])
        self.assertEqual(lock.events, ['release'])  # Sleep cannot be vetoed; the kernel refuses it.
        self.assertEqual((service.phase, service.sleeping), ('connected', False))
        self.assertEqual(kernel.notified.count(d.BUSY_FOR_SLEEP), 1)
        stores = list(kernel.stores)
        self.assertEqual(service.sleep_request('pre'), (False, d.BUSY_FOR_SLEEP))
        self.assertEqual((kernel.stores, kernel.notified.count(d.BUSY_FOR_SLEEP)), (stores, 1))
        self.assertEqual(kernel.unmounted, [])  # The hook did not try again.
        d.answer_logind(service, lock, [False])
        kernel.desktop_busy = kernel.unmount_busy = False
        self.assertEqual(service.sleep_request('pre')[0], True)  # A new sleep starts afresh.
        self.assertEqual(service.phase, 'ejected-for-sleep')

    def test_old_refusal_is_not_reused(self):
        kernel, service = connected()
        now = [0.0]
        service.router.clock = lambda: now[0]
        kernel.desktop_busy = kernel.unmount_busy = True
        d.answer_logind(service, FakeLock(), [True])
        now[0] += d.REFUSAL_REUSE + 1
        kernel.unmount_busy = False
        self.assertEqual(service.sleep_request('pre')[0], True)
        self.assertEqual(kernel.unmounted, ['nvme1n1p4'])

    def test_sleep_already_over_only_resumes(self):
        kernel, service = connected()
        lock = FakeLock()
        d.answer_logind(service, lock, [True, False])
        self.assertEqual((service.phase, kernel.desktop_unmounted, lock.events), ('connected', [], ['take']))
        d.answer_logind(service, lock, [False, True])
        self.assertEqual(service.phase, 'ejected-for-sleep')
        self.assertEqual(lock.events, ['take', 'take', 'release'])

    def test_idle_router_powers_down_at_the_announcement(self):
        kernel, service = make()
        service.step()
        lock = FakeLock()
        d.answer_logind(service, lock, [True])
        self.assertEqual((service.phase, lock.events), ('asleep', ['release']))
        self.assertEqual(names(kernel), ['inventory_once', 'idle_retire_once'])
        self.assertTrue(service.sleep_request('pre')[0])
        self.assertEqual(names(kernel), ['inventory_once', 'idle_retire_once'])


class FakeStdin:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


class FakeProcess:
    def __init__(self, args, stdout=None):
        self.args = args
        self.stdin = FakeStdin()
        self.stdout = stdout
        self.returncode = None

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is None:
            assert self.stdin.closed, 'would block'  # cat ends when our pipe closes.
            self.returncode = 0
        return self.returncode

    def terminate(self):
        self.returncode = -15

    def kill(self):
        self.returncode = -9


class Spawner:
    def __init__(self):
        self.processes = []
        self.writers = []
        self.fail = False

    def __call__(self, args, **options):
        if self.fail:
            raise OSError(errno.ENOENT, 'missing')
        stdout = None
        if args[0] == '/usr/bin/dbus-monitor':
            read, write = os.pipe()
            stdout = os.fdopen(read, 'rb')
            self.writers.append(write)
        process = FakeProcess(args, stdout)
        self.processes.append(process)
        return process

    def named(self, program):
        return [p for p in self.processes if p.args[0] == program]

    def close(self):
        for write in self.writers:
            try:
                os.close(write)
            except OSError:
                pass


HEADER = ('signal time=1790137141.651250 sender=:1.5 -> destination=(null destination) serial=812 '
          'path=/org/freedesktop/login1; interface=org.freedesktop.login1.Manager; member=PrepareForSleep\n')
NAME = ('signal time=1790137141.651250 sender=org.freedesktop.DBus -> destination=:1.99 serial=4294967295 '
        'path=/org/freedesktop/DBus; interface=org.freedesktop.DBus; member=NameAcquired\n   string ":1.99"\n')


class Monitor(unittest.TestCase):
    def setUp(self):
        self.now = [100.0]
        self.pauses = []
        self.spawn = Spawner()
        self.logind = d.Logind(spawn=self.spawn, clock=lambda: self.now[0], pause=self.pauses.append)
        self.addCleanup(self.spawn.close)
        self.addCleanup(self.logind.close)

    def write(self, text):
        os.write(self.spawn.writers[-1], text.encode())

    def test_lock_and_monitor_use_the_distribution_tools(self):
        self.assertEqual(self.logind.wait(0), [])
        monitor, = self.spawn.named('/usr/bin/dbus-monitor')
        lock, = self.spawn.named('/usr/bin/systemd-inhibit')
        self.assertEqual(monitor.args, list(d.MONITOR))
        self.assertIn("sender='org.freedesktop.login1'", monitor.args[2])
        self.assertEqual(lock.args[1:5], ['--what=sleep', '--mode=delay', '--who=USB4',
                                          '--why=Eject the USB4 drive before sleep'])
        self.assertEqual(lock.args[-1], '/usr/bin/cat')

    def test_only_login1_announcements_count_even_split_across_reads(self):
        self.logind.wait(0)
        self.write(NAME + HEADER + '   bool')
        self.assertEqual(self.logind.wait(1), [])
        self.write('ean true\n' + NAME.replace('string ":1.99"', 'boolean true') + HEADER + '   boolean false\n')
        self.assertEqual(self.logind.wait(1), [True, False])  # NameAcquired's boolean is not one.

    def test_release_closes_the_pipe_and_take_starts_a_new_lock(self):
        self.logind.wait(0)
        first, = self.spawn.named('/usr/bin/systemd-inhibit')
        self.logind.release()
        self.assertTrue(first.stdin.closed)
        self.assertEqual(first.returncode, 0)
        self.logind.wait(0)
        self.assertEqual(len(self.spawn.named('/usr/bin/systemd-inhibit')), 1)  # Released until resume.
        self.logind.take()
        self.assertEqual(len(self.spawn.named('/usr/bin/systemd-inhibit')), 2)

    def test_monitor_end_drops_the_lock_and_restarts_later(self):
        self.logind.wait(0)
        lock, = self.spawn.named('/usr/bin/systemd-inhibit')
        os.close(self.spawn.writers[-1])
        self.assertEqual(self.logind.wait(1), [])
        self.assertTrue(lock.stdin.closed)  # A lock nobody acts on would only delay sleep.
        self.assertEqual(self.logind.wait(0.5), [])
        self.assertEqual((self.pauses, len(self.spawn.processes)), ([0.5], 2))
        self.now[0] += 10
        self.logind.wait(0)
        self.assertEqual(len(self.spawn.named('/usr/bin/dbus-monitor')), 2)
        self.assertEqual(len(self.spawn.named('/usr/bin/systemd-inhibit')), 2)

    def test_lock_that_ended_on_its_own_is_taken_again(self):
        self.logind.wait(0)
        lock, = self.spawn.named('/usr/bin/systemd-inhibit')
        lock.returncode = 1
        self.logind.wait(0)
        self.assertEqual(len(self.spawn.named('/usr/bin/systemd-inhibit')), 2)
        self.spawn.named('/usr/bin/systemd-inhibit')[1].returncode = 1
        self.logind.wait(0)
        self.assertEqual(len(self.spawn.named('/usr/bin/systemd-inhibit')), 2)  # At most every 10 s.

    def test_missing_tools_leave_the_hook_path(self):
        self.spawn.fail = True
        self.assertEqual(self.logind.wait(0.5), [])
        self.assertEqual(self.pauses, [0.5])
        self.assertIsNone(self.logind.lock)


class Desktop(unittest.TestCase):
    def test_desktop_commands_run_in_the_account_session(self):
        calls = []

        def run(command, **options):
            calls.append((command, options))
            return subprocess.CompletedProcess(command, 0)
        user = mock.Mock(pw_uid=1000)
        with mock.patch.object(d.subprocess, 'run', run), mock.patch.object(d.pwd, 'getpwnam', return_value=user):
            kernel = d.Kernel('birk')
            self.assertTrue(kernel.unmount_desktop('/run/media/birk/My Drive'))
            kernel.notify_text('hello')
        env = {'PATH': '/usr/bin:/bin', 'XDG_RUNTIME_DIR': '/run/user/1000',
               'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/user/1000/bus'}
        self.assertEqual(calls[0][0], ['/usr/bin/runuser', '-u', 'birk', '--', '/usr/bin/gio', 'mount',
                                       '--unmount', '/run/media/birk/My Drive'])
        self.assertEqual((calls[0][1]['env'], calls[0][1]['timeout']), (env, d.DESKTOP_UNMOUNT))
        self.assertEqual(calls[1][0], ['/usr/bin/runuser', '-u', 'birk', '--', '/usr/bin/notify-send',
                                       '--app-name=USB4', 'USB4', 'hello'])
        self.assertEqual(calls[1][1]['env'], env)

    def test_desktop_unmount_timeout_or_failure_is_false(self):
        def slow(command, **options):
            raise subprocess.TimeoutExpired(command, options['timeout'])
        user = mock.Mock(pw_uid=1000)
        with mock.patch.object(d.pwd, 'getpwnam', return_value=user):
            with mock.patch.object(d.subprocess, 'run', slow):
                self.assertFalse(d.Kernel('birk').unmount_desktop('/x'))
            with mock.patch.object(d.subprocess, 'run',
                                   lambda c, **o: subprocess.CompletedProcess(c, 2)):
                self.assertFalse(d.Kernel('birk').unmount_desktop('/x'))


class Ports(unittest.TestCase):
    def test_other_port_in_use_does_not_block_this_router(self):
        # Chargers and hubs on the other connectors are invisible to this router's session.
        kernel, service = make()
        service.step()
        self.assertEqual(service.phase, 'waiting')
        self.assertFalse(hasattr(kernel, 'ports_empty'))


class Protocol(unittest.TestCase):
    def test_status_advertises_sleep_and_start_requires_new_stages(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            params = root / 'sys/module/thunderbolt/parameters'
            params.mkdir(parents=True)
            for name in d.REQUIRED:
                (params / name).write_text('Y\n')
            for name in d.FORBIDDEN:
                (params / name).write_text('N\n')
            platform = root / 'sys/bus/platform/devices/15600000.usb4'
            platform.mkdir(parents=True)
            kernel = d.Kernel('birk', root=root)
            with self.assertRaises(d.Stop):
                kernel.parameters_ok()  # An omarchy3 kernel: no idle or pulled stage.
            for name in d.STAGES:
                (platform / name).write_text('')
            kernel.parameters_ok()
            (root / 'run').mkdir()
            kernel.publish(dict(phase='waiting', generation=1, disk=None, error=None))
            status = __import__('json').loads((root / 'run/omarchy-usb4/status.json').read_text())
            self.assertEqual(status['sleep'], d.SLEEP_PROTOCOL)


class Parsing(unittest.TestCase):
    def test_multiline_snapshot(self):
        self.assertEqual(d.fields('busy=0 a=1\nb=2 c=x\n'), dict(busy='0', a='1', b='2', c='x'))

    def test_mounted_matches_disk_and_partitions_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            block = root / 'sys/block/nvme1n1'
            (block / 'nvme1n1p4').mkdir(parents=True)
            (block / 'dev').write_text('259:6\n')
            (block / 'nvme1n1p4/dev').write_text('259:7\n')
            (root / 'proc/self').mkdir(parents=True)
            (root / 'proc/self/mountinfo').write_text(
                '361 34 259:7 / /run/media/birk/LACIE rw - ext4 /dev/nvme1n1p4 rw\n'
                '362 34 259:7 / /run/media/birk/My\\040Drive\\134x rw - ext4 /dev/nvme1n1p4 rw\n'
                '30 1 259:2 / / rw - btrfs /dev/mapper/root rw\n')
            kernel = d.Kernel('birk', root=root)
            self.assertEqual(kernel.mounted('nvme1n1'), ['/run/media/birk/LACIE', '/run/media/birk/My Drive\\x'])
            self.assertEqual(kernel.mounted_devices('nvme1n1'), ['nvme1n1p4'])


if __name__ == '__main__':
    unittest.main()
