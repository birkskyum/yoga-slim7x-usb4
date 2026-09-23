#!/usr/bin/env python3
"""USB4 storage service for the Yoga Slim 7x (thunderbolt x1_general flavor, v0).

Drives the X1 router through its managed kernel stages. It prepares the router
while the left ports are empty, connects when a drive is plugged in, and once
every filesystem on the drive is unmounted it removes the NVMe endpoint and
powers the router down so the cable can be pulled. Files mounts and unmounts
the drive through udisks like any external disk.

Before system sleep it powers the router down: an idle router directly, a
connected drive after unmounting and ejecting it. This starts when logind
announces sleep, while the desktop still runs, so an open Files window lets go
of the drive as it does for Eject in Files; the systemd-sleep hook is the
fallback. The laptop does not sleep while files on the drive are still open. A
drive pulled without Eject is retired without touching it, and the next drive
connects without a restart.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import select
import subprocess
import sys
import time

PLATFORM = Path('/sys/bus/platform/devices/15600000.usb4')
PARAMS = Path('/sys/module/thunderbolt/parameters')
HOST_BRIDGE = '400000000.pcie-imsi'
RUN = Path('/run/omarchy-usb4')
RULE = Path('/run/udev/rules.d/90-omarchy-usb4.rules')
RULE_TEXT = ('# Drives behind the Yoga USB4 tunnel are external disks, like USB drives.\n'
             'SUBSYSTEM=="block", KERNELS=="' + HOST_BRIDGE + '", ENV{UDISKS_SYSTEM}="0"\n')
REQUIRED = ('x1_general', 'x1_managed', 'x1_inventory_batch', 'x1_imsi_receiver', 'x1_nvme_read',
            'x1_tunnel_quiesce', 'x1_host_quiesce', 'x1_power_quiesce', 'x1_namespace_retire',
            'x1_pci_retire', 'x1_control_retire', 'x1_cm_retire')
FORBIDDEN = ('x1_startup', 'x1_runtime_startup', 'x1_pcie_prepare', 'x1_pcie_inspect', 'x1_pcie_cfg0')
STAGES = ('idle_retire_once', 'pulled_retire_once')  # Kernels without these predate this service.
SLEEP_PROTOCOL = 1
INVENTORY = b'inventory any\n'
IDLE_RETIRE = b'retire-idle-v1\n'
PULLED_RETIRE = b'retire-pulled-v1\n'
SLEEP_BUDGET = 60  # seconds; systemd-sleep gives each hook 90
SLEEP_GRACE = 120  # monotonic seconds without a post hook before resuming anyway
REFUSAL_REUSE = 60  # seconds a refusal from logind's announcement answers the hook
DESKTOP_UNMOUNT = 10  # seconds for Files to let go and the desktop unmount to finish
MONITOR = ('/usr/bin/dbus-monitor', '--system',
           "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',"
           "member='PrepareForSleep'")
INHIBIT = ('/usr/bin/systemd-inhibit', '--what=sleep', '--mode=delay', '--who=USB4',
           '--why=Eject the USB4 drive before sleep', '/usr/bin/cat')
ANNOUNCEMENT = 'interface=org.freedesktop.login1.Manager; member=PrepareForSleep'
EJECT = (('endpoint_remove_once', b'remove-endpoint-v1\n', 'general_state',
          dict(endpoint_removed=1, endpoint_remove_error=0)),
         ('tunnel_quiesce_once', b'stop-retain-v1\n', 'tunnel_quiesce_state',
          dict(attempted=1, complete=1, error=0)),
         ('host_quiesce_once', b'stop-host-retain-v1\n', 'host_quiesce_state',
          dict(attempted=1, complete=1, error=0)),
         ('power_quiesce_once', b'stop-power-retain-v1\n', 'power_quiesce_state',
          dict(attempted=1, complete=1, error=0)))
MESSAGES = {
    'starting': 'Starting USB4.',
    'unplug-to-start': 'Unplug the cable from the rear left USB-C port once so USB4 can start.',
    'waiting': 'USB4 is ready. Connect a drive to the rear left USB-C port.',
    'connecting': 'Connecting the USB4 drive.',
    'replug-required': 'USB4 did not start on this connection. Unplug the cable, wait two seconds, '
                       'and plug it in again.',
    'connected': 'The USB4 drive is connected. Open it in Files, and eject it there before unplugging.',
    'releasing': 'Waiting for the system to finish with the USB4 drive. Do not unplug yet.',
    'ejecting': 'Finishing with the USB4 drive. Do not unplug yet.',
    'safe-to-unplug': 'The USB4 drive can be unplugged now.',
    'failed': 'USB4 stopped. Restart the laptop to use USB4 again.',
    'asleep': 'USB4 is off while the laptop sleeps.',
    'pulled': 'The USB4 drive was unplugged without ejecting. Files that were still being written may be '
              'incomplete.',
    'replug-after-pull': 'Unplug the USB4 drive and plug it in again to use it.',
    'ejected-for-sleep': 'The USB4 drive was ejected for sleep. Unplug it and plug it in again to use it.',
}
BUSY_FOR_SLEEP = ('The laptop cannot sleep while files on the USB4 drive are open. '
                  'Close them, or eject the drive in Files.')


class Stop(Exception):
    """A kernel receipt or step disagreed with the expected state."""


class Busy(Exception):
    """The drive is still in use; nothing was changed."""


def require(ok, message):
    if not ok:
        raise Stop(message)


def unescape(field):
    """A mountinfo path: the kernel writes space, tab, newline and backslash as octal escapes."""
    return re.sub(r'\\([0-7]{3})', lambda match: chr(int(match.group(1), 8)), field)


def fields(text):
    value = {}
    for line in text.splitlines():
        for item in line.split():
            key, _, data = item.partition('=')
            value[key] = data
    return value


class Kernel:
    """Everything that touches sysfs, udev or the desktop session."""

    def __init__(self, account, root=Path('/')):
        self.account = account
        self.root = root

    def path(self, path):
        return self.root / path.relative_to('/')

    def read(self, name, retries=100):
        # Snapshots use trylocks; a Type-C callback holding the lock is not a fault.
        for _ in range(retries):
            value = fields(self.path(PLATFORM / name).read_text())
            if value.get('busy') != '1':
                require(value.get('busy') == '0', 'Malformed snapshot: ' + name)
                return value
            time.sleep(0.05)
        raise Stop('Snapshot stayed busy: ' + name)

    def store(self, name, token):
        fd = os.open(self.path(PLATFORM / name), os.O_WRONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            require(os.write(fd, token) == len(token), 'Short store: ' + name)
        finally:
            os.close(fd)

    def parameters_ok(self):
        for name in REQUIRED + FORBIDDEN:
            value = self.path(PARAMS / name).read_text().strip()
            require(value == ('Y' if name in REQUIRED else 'N'), 'Wrong thunderbolt option: ' + name)
        for name in STAGES:
            require(self.path(PLATFORM / name).exists(), 'Kernel lacks the USB4 stage ' + name)

    def install_rule(self):
        rule = self.path(RULE)
        rule.parent.mkdir(parents=True, exist_ok=True)
        if not rule.exists() or rule.read_text() != RULE_TEXT:
            rule.write_text(RULE_TEXT)
            subprocess.run(['/usr/bin/udevadm', 'control', '--reload'], check=False, timeout=10)

    def disk(self):
        """The whole-disk block device behind the tunnel host, if published."""
        for block in sorted(self.path(Path('/sys/block')).glob('nvme*n*')):
            if HOST_BRIDGE in str(block.resolve()):
                return block.name
        return None

    def mounted(self, disk):
        """Mount points of the tunnel disk and its partitions in this namespace."""
        numbers = set()
        base = self.path(Path('/sys/block')) / disk
        for entry in [base, *base.glob(disk + 'p*')]:
            dev = entry / 'dev'
            if dev.exists():
                numbers.add(dev.read_text().strip())
        points = []
        for line in self.path(Path('/proc/self/mountinfo')).read_text().splitlines():
            parts = line.split()
            if len(parts) > 4 and parts[2] in numbers:
                points.append(unescape(parts[4]))
        return points

    def mounted_devices(self, disk):
        """Names of the tunnel disk and partitions mounted in this namespace."""
        names = {}
        base = self.path(Path('/sys/block')) / disk
        for entry in [base, *base.glob(disk + 'p*')]:
            dev = entry / 'dev'
            if dev.exists():
                names[dev.read_text().strip()] = entry.name
        found = []
        for line in self.path(Path('/proc/self/mountinfo')).read_text().splitlines():
            parts = line.split()
            if len(parts) > 4 and parts[2] in names and names[parts[2]] not in found:
                found.append(names[parts[2]])
        return found

    def unmount(self, device):
        """Unmount through udisks, like Eject in Files. Never forced or lazy."""
        result = subprocess.run(['/usr/bin/udisksctl', 'unmount', '--no-user-interaction',
                                 '--block-device', '/dev/' + device],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=30, check=False)
        return result.returncode == 0

    def unmount_desktop(self, point):
        """Unmount through the desktop session, like Eject in Files: Files windows let go first.

        Needs a running user session, so only logind's announcement uses it. Never
        forced; False if anything still holds the mount or the session is absent.
        """
        try:
            command, env = self.desktop()
            result = subprocess.run(command + ['/usr/bin/gio', 'mount', '--unmount', point], env=env,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, timeout=DESKTOP_UNMOUNT, check=False)
        except (OSError, KeyError, subprocess.SubprocessError):
            return False
        return result.returncode == 0

    def released(self, disk):
        """True once no mount in any namespace still claims the disk or its partitions.

        A mounted filesystem holds its block device exclusively, so an O_EXCL
        open fails with EBUSY while a copy of the mount survives elsewhere.
        """
        base = self.path(Path('/sys/block')) / disk
        for entry in [base, *base.glob(disk + 'p*')]:
            try:
                fd = os.open(self.path(Path('/dev')) / entry.name, os.O_RDONLY | os.O_EXCL | os.O_CLOEXEC)
            except OSError as error:
                if error.errno == errno.EBUSY:
                    return False
                raise
            os.close(fd)
        return True

    def settle(self):
        subprocess.run(['/usr/bin/udevadm', 'settle', '--timeout=15'], check=False, timeout=20)

    def notify(self, phase):
        self.notify_text(MESSAGES[phase])

    def desktop(self):
        """Command prefix and environment that run a program in the account's desktop session."""
        runtime = '/run/user/' + str(pwd.getpwnam(self.account).pw_uid)
        return (['/usr/bin/runuser', '-u', self.account, '--'],
                {'PATH': '/usr/bin:/bin', 'XDG_RUNTIME_DIR': runtime,
                 'DBUS_SESSION_BUS_ADDRESS': 'unix:path=' + runtime + '/bus'})

    def notify_text(self, text):
        # Best effort only; never changes controller, disk or session state.
        try:
            command, env = self.desktop()
            subprocess.run(command + ['/usr/bin/notify-send', '--app-name=USB4', 'USB4', text], env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=5, check=False)
        except (OSError, KeyError, subprocess.SubprocessError):
            pass

    def publish(self, value):
        run = self.path(RUN)
        run.mkdir(mode=0o755, exist_ok=True)
        temporary = run / ('status-' + secrets.token_hex(8) + '.tmp')
        temporary.write_text(json.dumps(dict(value, message=MESSAGES[value['phase']], sleep=SLEEP_PROTOCOL,
                                             updated_ns=time.time_ns())) + '\n')
        os.chmod(temporary, 0o644)
        os.replace(temporary, run / 'status.json')


class Router:
    """The managed kernel stages for one generation at a time."""

    def __init__(self, kernel, clock=time.monotonic, sleep=time.sleep):
        self.kernel = kernel
        self.clock = clock
        self.sleep = sleep
        self.failures = 0

    def session(self, generation=None):
        value = self.kernel.read('session_state')
        require(value.get('managed') == '1', 'Router session is not managed')
        if generation is not None:
            require(value.get('generation') == str(generation), 'Kernel generation changed')
        return value

    def port_empty(self):
        """Only this router's own connector matters; the other ports are independent."""
        return self.session().get('orientation') == '0'

    def adopt(self):
        """What a stopped service left behind with the port empty: ('waiting' or 'retired', generation).

        None for a fresh session and for anything attached, connecting or half
        stopped; those keep the old rule, a restart of the laptop.
        """
        value = self.session()
        if value.get('orientation') != '0' or value.get('pristine') == '1':
            return None
        if value.get('fully_retired') == '1':
            if value.get('platform_retired') == '1' and value.get('platform_error') == '0':
                return 'retired', int(value['generation'])
            return None
        if not (value.get('prepared') == '1' and value.get('waiting') == '1' and value.get('complete') == '0'):
            return None
        inventory = self.kernel.read('inventory_state')
        if (inventory.get('error'), inventory.get('terminal'), inventory.get('step'),
                inventory.get('negotiation_rearm'), inventory.get('negotiation_failures')) != \
                ('0', '0', 'connect-once', '0', '0'):
            return None
        return 'waiting', int(value['generation'])

    def prepare(self, old):
        value = self.session()
        generation = int(value['generation'])
        require(generation > old, 'Generation not fresh')
        require(value.get('pristine') == '1' and value.get('orientation') == '0' and
                value.get('prepared') == '0' and value.get('fully_retired') == '0',
                'Router not pristine')
        self.failures = 0
        self.kernel.store('inventory_once', INVENTORY)
        value = self.session(generation)
        require(value.get('prepared') == '1' and value.get('waiting') == '1', 'Prepare incomplete')
        return generation

    def connection(self, generation):
        value = self.session(generation)
        require(value.get('platform_error') == '0', 'Platform retirement failed')
        if value.get('fully_retired') == '1':
            if value.get('orientation') == '0' and \
               int(value['disconnect_event']) > int(value['initial_event']):
                return 'disconnected'
            return 'retired-attached'
        inventory = self.kernel.read('inventory_state')
        require(inventory.get('error') == '0', 'Controller operation failed')
        if inventory.get('negotiation_rearm') == '1':
            return 'abandoned'
        if value.get('orientation') == '0':
            return 'empty'
        return 'connected' if value.get('complete') == '1' else 'ready'

    def rearmed(self, generation):
        value = self.session(generation)
        inventory = self.kernel.read('inventory_state')
        failures = inventory.get('negotiation_failures', '')
        return (value.get('waiting') == '1' and value.get('complete') == '0' and
                inventory.get('terminal') == '0' and inventory.get('error') == '0' and
                inventory.get('step') == 'connect-once' and failures.isdecimal() and
                int(failures) == self.failures + 1)

    def connect(self, generation):
        try:
            self.kernel.store('inventory_once', INVENTORY)
        except OSError as error:
            if error.errno != errno.ENOTCONN or not self.rearmed(generation):
                raise
            self.failures += 1
            return 'replug-required'
        value = self.session(generation)
        require(value.get('complete') == '1' and value.get('waiting') == '0', 'Connect incomplete')
        deadline = self.clock() + 45
        while self.kernel.disk() is None:
            require(self.clock() < deadline, 'The drive did not appear')
            self.sleep(0.2)
        self.kernel.settle()
        return 'connected'

    def eject(self, generation):
        for store, token, snapshot, wanted in EJECT:
            self.kernel.store(store, token)
            value = self.kernel.read(snapshot)
            require(all(value.get(k) == str(v) for k, v in wanted.items()), 'Eject step failed: ' + store)
        value = self.session(generation)
        require(value.get('fully_retired') == '1' and value.get('platform_retired') == '1',
                'Router not fully retired')
        require(self.kernel.disk() is None, 'The drive is still visible')

    def renew(self, generation):
        self.kernel.store('renew_session', (str(generation) + '\n').encode())

    def pulled_retire(self, generation):
        """Retire a connected router whose drive was pulled; nothing touches the drive."""
        self.kernel.store('pulled_retire_once', PULLED_RETIRE)
        state = self.kernel.read('general_state')
        require(state.get('pulled_retired') == '1' and state.get('pulled_retire_error') == '0',
                'Pulled drive retirement incomplete')
        value = self.session(generation)
        require(value.get('fully_retired') == '1' and value.get('platform_retired') == '1',
                'Router not fully retired')

    def idle_retire(self, generation):
        """Power down a prepared router with no connection. False if refused unchanged."""
        try:
            self.kernel.store('idle_retire_once', IDLE_RETIRE)
        except OSError as error:
            state = self.kernel.read('general_state')
            require(state.get('idle_retire_attempted') == '0',
                    'USB4 power-down failed at ' + state.get('idle_retire_stage', 'unknown'))
            if error.errno in (errno.EBUSY, errno.EPERM):
                return False
            raise
        state = self.kernel.read('general_state')
        require(state.get('idle_retired') == '1' and state.get('idle_retire_error') == '0',
                'USB4 power-down incomplete')
        value = self.session(generation)
        require(value.get('fully_retired') == '1' and value.get('platform_retired') == '1',
                'Router not fully retired')
        return True


class Service:
    """Phase machine. Kernel effects go through Router and Kernel."""

    def __init__(self, router, kernel):
        self.router = router
        self.kernel = kernel
        self.phase = 'starting'
        self.generation = 0
        self.disk = None
        self.was_mounted = False
        self.error = None
        self.idle_retired = False
        self.sleeping = False
        self.sleep_started = 0.0
        self.refusal = None  # (message, time) of a sleep refused at logind's announcement

    def publish(self, phase, notify=True):
        changed = phase != self.phase
        self.phase = phase
        self.kernel.publish(dict(phase=phase, generation=self.generation, disk=self.disk,
                                 error=self.error))
        if changed and notify:
            self.kernel.notify(phase)

    def fail(self, error):
        self.error = str(error)
        self.publish('failed')

    def start(self):
        self.kernel.parameters_ok()
        self.kernel.install_rule()
        self.publish('starting', notify=False)

    def step(self):
        """One poll; returns False once the service has failed."""
        if self.phase == 'failed':
            return False
        try:
            self._step()
        except (Stop, OSError, ValueError, KeyError) as error:
            self.fail(error)
            return False
        return True

    def _step(self):
        if self.sleeping:
            # Nothing may power the router up between the sleep hook and resume.
            if self.router.clock() - self.sleep_started < SLEEP_GRACE:
                return
            self.sleeping = False
        if self.phase in ('starting', 'unplug-to-start', 'asleep'):
            adopted = self.router.adopt() if self.generation == 0 else None
            if adopted is not None:  # After a service restart, for example an upgrade.
                state, generation = adopted
                if state == 'retired':
                    try:
                        self.router.renew(generation)
                    except OSError as error:
                        if error.errno != errno.EBUSY:
                            raise
                        self.publish('unplug-to-start')  # The kernel still sees the drive.
                        return
                    generation = self.router.prepare(generation)
                self.generation = generation
                self.publish('waiting', notify=self.phase == 'unplug-to-start')
                return
            if not self.router.port_empty():
                self.publish('unplug-to-start')
                return
            old = self.generation
            if self.idle_retired:
                try:
                    self.router.renew(old)
                except OSError as error:
                    if error.errno != errno.EBUSY:
                        raise
                    self.publish('unplug-to-start')  # A cable arrived meanwhile.
                    return
                self.idle_retired = False
            self.generation = self.router.prepare(old)
            self.publish('waiting', notify=self.phase == 'unplug-to-start')
            return
        status = self.router.connection(self.generation)
        if self.phase in ('connected', 'releasing') and status == 'empty':
            # Pulled without Eject: retire without touching the drive.
            self.publish('pulled')
            self.router.pulled_retire(self.generation)
            self.disk = None
        elif self.phase == 'waiting' and status == 'ready':
            self.connect()
        elif self.phase == 'replug-required':
            if status in ('empty', 'ready'):
                self.publish('waiting', notify=False)
        elif self.phase in ('connected', 'releasing'):
            require(status == 'connected', 'Unexpected USB4 state while connected: ' + status)
            if self.kernel.mounted(self.disk):
                self.was_mounted = True
                if self.phase == 'releasing':
                    self.publish('connected', notify=False)
            elif self.was_mounted:
                # A mount copy in another namespace keeps the filesystem open.
                if not self.kernel.released(self.disk):
                    self.publish('releasing', notify=False)
                    return
                self.publish('ejecting')
                self.router.eject(self.generation)
                self.disk = None
                self.publish('safe-to-unplug')
        elif self.phase in ('pulled', 'replug-after-pull') and status == 'retired-attached':
            self.publish('replug-after-pull')
        elif self.phase in ('safe-to-unplug', 'ejected-for-sleep', 'pulled', 'replug-after-pull') and \
                status == 'disconnected':
            old = self.generation
            self.router.renew(old)
            self.generation = self.router.prepare(old)
            self.publish('waiting', notify=False)

    def connect(self):
        self.publish('connecting', notify=False)
        if self.router.connect(self.generation) == 'replug-required':
            self.publish('replug-required')
            return
        self.disk = self.kernel.disk()
        self.was_mounted = False
        self.publish('connected')

    def sleep_pre(self, desktop=False):
        """Before system sleep: leave no router powered, or raise Busy unchanged."""
        deadline = self.router.clock() + SLEEP_BUDGET
        while True:
            if self.phase in ('starting', 'unplug-to-start', 'safe-to-unplug', 'ejected-for-sleep',
                              'asleep', 'pulled', 'replug-after-pull'):
                break  # Never started, or retired: the router holds nothing.
            require(self.phase != 'failed', 'USB4 had already stopped')
            if self.phase in ('waiting', 'replug-required'):
                if self.router.idle_retire(self.generation):
                    self.idle_retired = True
                    self.publish('asleep', notify=False)
                    break
                # Refused unchanged: a drive is connecting on this router right now.
                if self.phase == 'waiting' and self.router.connection(self.generation) == 'ready':
                    self.connect()
                    continue
            elif self.phase in ('connected', 'releasing'):
                self.release_for_sleep(deadline, desktop)
                self.publish('ejected-for-sleep')
                break
            require(self.router.clock() < deadline, 'USB4 did not power down in time for sleep')
            self.router.sleep(0.5)
        self.sleeping = True
        self.sleep_started = self.router.clock()

    def release_for_sleep(self, deadline, desktop=False):
        """Unmount through udisks, wait until no namespace holds the disk, then eject.

        With desktop, the session unmounts first, so Files windows let go of the
        drive; whatever it could not unmount goes to udisks as before.
        """
        if desktop:
            for point in self.kernel.mounted(self.disk):
                self.kernel.unmount_desktop(point)
        settled = 0
        while settled < 2:
            if self.router.clock() >= deadline:
                raise Busy(BUSY_FOR_SLEEP)
            devices = self.kernel.mounted_devices(self.disk)
            if devices:
                settled = 0
                for device in devices:
                    if not self.kernel.unmount(device):
                        raise Busy(BUSY_FOR_SLEEP)
                self.was_mounted = True
            elif self.kernel.released(self.disk):
                settled += 1
            else:
                settled = 0
                self.publish('releasing', notify=False)
            self.router.sleep(0.5)
        self.publish('ejecting')
        self.router.eject(self.generation)
        self.disk = None

    def sleep_post(self):
        self.sleeping = False
        self.refusal = None

    def sleep_request(self, action, desktop=False):
        """Handle one sleep request; returns (ok, message).

        The systemd-sleep hook sends 'pre' and 'post' with the user session
        frozen. logind's announcement comes first and sends 'pre' with desktop.
        """
        if action not in ('pre', 'post'):
            return False, 'Unknown sleep action'
        try:
            if action == 'pre':
                if not desktop and self.refusal and self.router.clock() - self.refusal[1] < REFUSAL_REUSE:
                    return False, self.refusal[0]  # Refused and shown moments ago at the announcement.
                self.sleep_pre(desktop)
            else:
                self.sleep_post()
        except Busy as error:
            if desktop:
                self.refusal = (str(error), self.router.clock())
            self.kernel.notify_text(str(error))
            return False, str(error)
        except (Stop, OSError, ValueError, KeyError) as error:
            self.fail(error)
            return False, str(error)
        return True, MESSAGES[self.phase]

    def eject_now(self):
        """Manual eject for drives that were never mounted."""
        require(self.phase in ('connected', 'releasing'), 'No connected drive to eject')
        require(not self.kernel.mounted(self.disk), 'Unmount the drive first')
        require(self.kernel.released(self.disk), 'The drive is still in use elsewhere')
        self.publish('ejecting')
        self.router.eject(self.generation)
        self.disk = None
        self.publish('safe-to-unplug')


def answer_sleep(service, run):
    try:
        request = json.loads((run / 'sleep-request').read_text())
    except FileNotFoundError:
        return
    except ValueError:
        request = {}
    (run / 'sleep-request').unlink()
    ok, message = service.sleep_request(str(request.get('action')))
    print('Sleep ' + str(request.get('action')) + ': ' + ('ok' if ok else 'refused') + ', ' + message,
          file=sys.stderr, flush=True)
    temporary = run / ('sleep-' + secrets.token_hex(8) + '.tmp')
    temporary.write_text(json.dumps(dict(nonce=request.get('nonce'), ok=ok, message=message,
                                         phase=service.phase)) + '\n')
    os.chmod(temporary, 0o644)
    os.replace(temporary, run / 'sleep-result')


class Logind:
    """logind's sleep announcements, and a delay lock that holds sleep back until USB4 is off.

    The same tools as Omarchy's lock-before-suspend: dbus-monitor reports
    PrepareForSleep and systemd-inhibit holds the lock while its cat reads our
    pipe. systemd-sleep freezes the user session before it runs sleep hooks, so
    the announcement is the last point where Files can still let go of a drive.
    """

    def __init__(self, spawn=subprocess.Popen, clock=time.monotonic, pause=time.sleep):
        self.spawn = spawn
        self.clock = clock
        self.pause = pause
        self.monitor = None
        self.lock = None
        self.buffer = b''
        self.announcing = False
        self.retry_at = 0.0
        self.lock_retry_at = 0.0

    def take(self):
        if self.lock is None:
            try:
                self.lock = self.spawn(list(INHIBIT), stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
            except OSError as error:
                print('No delay lock; sleep may start before USB4 is off: ' + str(error),
                      file=sys.stderr, flush=True)

    def release(self):
        """Let logind go on with sleep. Closing the pipe ends cat and with it the lock."""
        lock, self.lock = self.lock, None
        if lock is not None:
            lock.stdin.close()
            try:
                lock.wait(timeout=5)
            except subprocess.TimeoutExpired:
                lock.kill()  # Our own helper; the lock goes with it.
                lock.wait()

    def watch(self):
        """Start the monitor, then the lock: a lock nobody acts on would only delay sleep."""
        if self.monitor is not None and self.monitor.poll() is None:
            return True
        self.close()
        if self.clock() < self.retry_at:
            return False
        self.retry_at = self.clock() + 10
        try:
            self.monitor = self.spawn(list(MONITOR), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL)
            os.set_blocking(self.monitor.stdout.fileno(), False)
            self.take()
        except OSError as error:
            print('No sleep announcements, the sleep hook alone remains: ' + str(error),
                  file=sys.stderr, flush=True)
            self.close()
            return False
        self.buffer, self.announcing = b'', False
        return True

    def wait(self, timeout):
        """Wait up to timeout seconds; returns the PrepareForSleep values that arrived, in order."""
        if not self.watch():
            self.pause(timeout)
            return []
        if self.lock is not None and self.lock.poll() is not None and self.clock() >= self.lock_retry_at:
            self.lock_retry_at = self.clock() + 10
            self.release()  # It ended on its own; a released lock stays released until resume.
            self.take()
        ready, _, _ = select.select([self.monitor.stdout], [], [], timeout)
        if not ready:
            return []
        try:
            chunk = os.read(self.monitor.stdout.fileno(), 65536)
        except BlockingIOError:
            return []
        if not chunk:
            self.close()  # The monitor ended; the next wait restarts it.
            return []
        *lines, self.buffer = (self.buffer + chunk).split(b'\n')
        return self.parse(lines)

    def parse(self, lines):
        values = []
        for line in lines:
            text = line.decode(errors='replace')
            if text.startswith('signal '):
                self.announcing = ANNOUNCEMENT in text
            elif self.announcing and text.strip() in ('boolean true', 'boolean false'):
                values.append(text.strip() == 'boolean true')
                self.announcing = False
        return values

    def close(self):
        self.release()
        monitor, self.monitor = self.monitor, None
        if monitor is not None:
            if monitor.poll() is None:
                monitor.terminate()  # Our own dbus-monitor.
            try:
                monitor.wait(timeout=5)
            except subprocess.TimeoutExpired:
                monitor.kill()
                monitor.wait()
            monitor.stdout.close()


def answer_logind(service, logind, values):
    """Prepare for an announced sleep, then release the lock; take it again after resume.

    A sleep already followed by its resume is over: preparing for it now would
    only eject a drive after the laptop woke up.
    """
    for preparing in ([False] if False in values else []) + ([True] if values and values[-1] else []):
        ok, message = service.sleep_request('pre' if preparing else 'post', desktop=preparing)
        if preparing:
            logind.release()
        else:
            logind.take()
        print('logind ' + ('sleep' if preparing else 'resume') + ': ' + ('ok' if ok else 'refused') +
              ', ' + message, file=sys.stderr, flush=True)


def serve(account):
    kernel = Kernel(account)
    service = Service(Router(kernel), kernel)
    run = kernel.path(RUN)
    request = run / 'eject-request'
    service.start()
    logind = Logind()
    try:
        while service.step():
            if request.exists():
                request.unlink()
                try:
                    service.eject_now()
                except Stop as error:
                    print('Eject refused: ' + str(error), file=sys.stderr, flush=True)
            answer_sleep(service, run)
            answer_logind(service, logind, logind.wait(0.2 if service.sleeping else 0.5))
    finally:
        logind.close()
    print('STOP: ' + str(service.error), file=sys.stderr, flush=True)
    return 1


def sleep_hook(action):
    """systemd-sleep hook side: ask the running service and wait for its answer."""
    if subprocess.run(['/usr/bin/systemctl', 'is-active', '--quiet', 'omarchy-usb4.service'],
                      check=False).returncode != 0 or not RUN.is_dir():
        return 0
    try:
        protocol = json.loads((RUN / 'status.json').read_text()).get('sleep', 0)
    except (OSError, ValueError):
        protocol = 0
    if protocol != SLEEP_PROTOCOL:
        return 0  # An older service that does not answer sleep requests.
    nonce = secrets.token_hex(8)
    temporary = RUN / ('sleep-request-' + nonce + '.tmp')
    temporary.write_text(json.dumps(dict(action=action, nonce=nonce)) + '\n')
    os.replace(temporary, RUN / 'sleep-request')
    deadline = time.monotonic() + (SLEEP_BUDGET + 15 if action == 'pre' else 10)
    while time.monotonic() < deadline:
        try:
            result = json.loads((RUN / 'sleep-result').read_text())
        except (FileNotFoundError, ValueError):
            result = {}
        if result.get('nonce') == nonce:
            print('USB4 sleep ' + action + ': ' + result.get('message', ''), flush=True)
            return 0 if result.get('ok') else 1
        time.sleep(0.2)
    print('USB4 service did not answer the sleep ' + action + ' request', file=sys.stderr, flush=True)
    return 1


def main():
    os.environ['PATH'] = '/usr/bin:/bin'
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--serve', action='store_true')
    action.add_argument('--status', action='store_true')
    action.add_argument('--eject', action='store_true')
    action.add_argument('--sleep', choices=('pre', 'post'))
    parser.add_argument('--account', default='birk')
    args = parser.parse_args()
    if args.sleep:
        return sleep_hook(args.sleep)
    if args.status:
        print((RUN / 'status.json').read_text(), end='')
        return 0
    if args.eject:
        (RUN / 'eject-request').touch()
        return 0
    return serve(args.account)


if __name__ == '__main__':
    sys.exit(main())
