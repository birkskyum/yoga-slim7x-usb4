"""Private pre-unmount namespace settling. Metadata reads only; no I/O retry.

The frozen classifier remains authoritative. Never admit a conflicting mount:
wait briefly for temporary namespace copies to vanish, then run the unchanged
normal unmount once. Post-unmount checks never wait or waive a conflict.
"""
import json
import os
from pathlib import Path
import time

WAIT_SECONDS = 10.0
INTERVAL = 0.1
MAX_SCANS = 101


def details(conflicts, proc):
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
            value['comm'] = (path / 'comm').read_text()[:256].strip()
            value['cgroup'] = (path / 'cgroup').read_text()[:4096]
            # Only proc metadata: never follow cwd/fd links or open storage.
            rows = (path / 'mountinfo').read_text().splitlines()
            selected = [r for r in rows if len(r.split()) >= 6 and
                        r.split()[4] == conflict.get('mount')]
            parents = {r.split()[1] for r in selected}
            value['mountinfo'] = (selected + [r for r in rows if r.split()[0] in parents])[:16]
            value['namespace_stable'] = namespace == os.readlink(path / 'ns/mnt')
        except (OSError, ValueError) as error:
            value['observation_error'] = str(error)
        result.append(value)
    return result


def settle(scan, target, *, proc=Path('/proc'), allow_inherited=False,
           report=None, clock=time.monotonic, sleep=time.sleep, describe=details):
    # Keep retirement/post-unmount callers byte-for-byte in semantic scope:
    # one original scan, no waiting, no propagation exception added here.
    if not allow_inherited:
        return scan(target, proc=proc, allow_inherited=False)
    start = clock()
    deadline = start + WAIT_SECONDS
    history = []
    previous = None
    conflicts = []
    for number in range(MAX_SCANS):
        conflicts = scan(target, proc=proc, allow_inherited=True)
        if conflicts != previous:
            history.append(dict(elapsed_seconds=clock()-start,
                                conflicts=describe(conflicts, proc)))
            previous = conflicts
        if not conflicts or clock() >= deadline:
            break
        sleep(min(INTERVAL, max(0, deadline-clock())))
    if any(item['conflicts'] for item in history) and report is not None:
        report(dict(kind='namespace-pre-unmount-settle', scans=number+1,
                    elapsed_seconds=clock()-start, cleared=not conflicts,
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
