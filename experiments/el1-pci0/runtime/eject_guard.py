"""Private pre-unmount namespace settling. Metadata reads only; no I/O retry.

The frozen classifier stays authoritative and a conflicting mount is never
admitted: wait for temporary namespace copies to vanish, then run the unchanged
normal unmount once. Post-unmount checks never wait or waive a conflict.
"""
import json
import os
from pathlib import Path
import time

# Opening Files starts systemd-hostnamed (ProtectSystem=strict), whose read-only
# copy of the mount is a classifier conflict until its 30-second idle exit.
WAIT_SECONDS = 45.0
INTERVAL = 0.25
MAX_SCANS = int(WAIT_SECONDS / INTERVAL) + 1


def rows_for(text, mount):
    """The rows at one mountpoint plus their parents; mountinfo text only."""
    rows = [row.split() for row in text.splitlines()]
    selected = [row for row in rows if len(row) >= 6 and row[4] == mount]
    parents = {row[1] for row in selected}
    parent_rows = [row for row in rows if len(row) >= 6 and row[0] in parents]
    return [' '.join(row) for row in selected + parent_rows][:16]


def details(conflicts, proc):
    try:
        init_user = os.readlink(proc / '1/ns/user')
    except OSError as error:
        init_user = 'unavailable: ' + str(error)
    result = []
    for conflict in conflicts[:32]:
        value = dict(conflict)
        pid = conflict.get('pid')
        if type(pid) is not int or pid < 1:
            result.append(value)
            continue
        path = proc / str(pid)
        try:
            namespace = os.readlink(path / 'ns/mnt')
            value['observed_namespace'] = namespace
            value['user_namespace'] = os.readlink(path / 'ns/user')
            value['init_user_namespace'] = value['user_namespace'] == init_user
            value['comm'] = (path / 'comm').read_text()[:256].strip()
            value['cgroup'] = (path / 'cgroup').read_text()[:4096]
            # Only proc metadata: never follow cwd/fd links or open storage.
            value['mountinfo'] = rows_for((path / 'mountinfo').read_text(), conflict.get('mount'))
            value['namespace_stable'] = namespace == os.readlink(path / 'ns/mnt')
        except (OSError, ValueError) as error:
            value['observation_error'] = str(error)
        result.append(value)
    return result


def settle(scan, target, *, proc=Path('/proc'), allow_inherited=False,
           report=None, clock=time.monotonic, sleep=time.sleep, describe=details):
    # Retirement and post-unmount callers keep one original strict scan.
    if not allow_inherited:
        return scan(target, proc=proc, allow_inherited=False)
    start = clock()
    deadline = start + WAIT_SECONDS
    history = []
    previous = None
    conflicts = []
    number = 0
    for number in range(MAX_SCANS):
        conflicts = scan(target, proc=proc, allow_inherited=True)
        if conflicts != previous:
            history.append(dict(elapsed_seconds=clock() - start,
                                conflicts=describe(conflicts, proc)))
            previous = conflicts
        if not conflicts or clock() >= deadline:
            break
        sleep(min(INTERVAL, max(0, deadline - clock())))
    if any(item['conflicts'] for item in history) and report is not None:
        report(dict(kind='namespace-pre-unmount-settle', scans=number + 1,
                    elapsed_seconds=clock() - start, cleared=not conflicts,
                    final_conflicts=conflicts, observations=history,
                    unmount_attempted=False, forced=False))
    return conflicts


def install(session):
    original = session.foreign_mounts
    if getattr(original, '_namespace_settle', False) is True:
        raise ValueError('Namespace settling adapter already installed')

    def report(value):
        # Worker PID scopes each user-requested eject; exclusive root-private
        # evidence, never overwrite a previous request. No filesystem access
        # below the external mount.
        try:
            value['host_mountinfo'] = rows_for(Path('/proc/self/mountinfo').read_text(),
                                               str(session.MOUNT))
        except OSError as error:
            value['host_mountinfo'] = 'unavailable: ' + str(error)
        path = session.RUN / ('namespace-settle-' + str(os.getpid()) + '.json')
        with path.open('x') as stream:
            os.fchmod(stream.fileno(), 0o600)
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())

    def guarded(target, proc=Path('/proc'), allow_inherited=False):
        return settle(original, target, proc=proc, allow_inherited=allow_inherited,
                      report=report)
    guarded._namespace_settle = True
    session.foreign_mounts = guarded
