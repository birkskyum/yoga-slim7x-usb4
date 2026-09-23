"""Conservative pre-unmount propagation classifier; no mount syscalls.

Only direct peers/slaves with matching parent topology are admitted. Indirect
chains, invisible parents, aliases and overmounts remain conflicts. Snapshot
inspection is not exclusion against concurrent privileged mount changes.
"""


def parse(text):
    rows = []
    for line in text.splitlines():
        f = line.split()
        split = f.index('-')
        if split < 6 or len(f) != split + 4:
            raise ValueError('Malformed mountinfo')
        tags = {}
        for tag in f[6:split]:
            key, sep, value = tag.partition(':')
            if key in tags:
                raise ValueError('Duplicate mountinfo tag')
            tags[key] = value if sep else True
        rows.append(dict(id=int(f[0]), parent=int(f[1]), dev=f[2], root=f[3],
                         path=f[4], options=f[5], tags=tags, fs=f[split+1],
                         source=f[split+2], super=f[split+3]))
    if len({r['id'] for r in rows}) != len(rows):
        raise ValueError('Duplicate mount ID')
    return rows


def identical(a, b):
    return all(a[k] == b[k] for k in ('dev', 'root', 'path', 'fs', 'source', 'super'))


def direct_receiver(source, recipient):
    group = source['tags'].get('shared')
    if not isinstance(group, str) or not group.isdecimal() or int(group) < 1:
        return False
    # Reject hidden propagation and unknown flags, even if a group matches.
    if set(recipient['tags']) - {'shared', 'master'}:
        return False
    return (recipient['tags'].get('shared') == group or
            recipient['tags'].get('master') == group)


def inherited_only(host, other, devices, path):
    """True only when every scoped mount is one direct propagated copy.

    Check the *parent's* propagation too: unmount events propagate from the
    parent, not merely from a matching target peer group. Reject all descendants
    and stacked mounts; normal umount2 still decides whether users are busy.
    """
    origins = [r for r in host if r['dev'] in devices]
    targets = [r for r in other if r['dev'] in devices]
    if len(origins) != 1 or len(targets) != 1:
        return False
    a, b = origins[0], targets[0]
    if a['path'] != path or a['root'] != '/' or a['fs'] != 'ext4':
        return False
    if not identical(a, b) or a['options'] != b['options'] or not direct_receiver(a, b):
        return False
    for rows, selected in ((host, a), (other, b)):
        if sum(r['path'] == path for r in rows) != 1:
            return False
        if any(r['path'].startswith(path + '/') for r in rows):
            return False
        if any(r['parent'] == selected['id'] for r in rows):
            return False
    host_parents = [r for r in host if r['id'] == a['parent']]
    other_parents = [r for r in other if r['id'] == b['parent']]
    if len(host_parents) != 1 or len(other_parents) != 1:
        return False
    ap, bp = host_parents[0], other_parents[0]
    # Systemd legitimately makes a recipient parent read-only/nosuid.
    return identical(ap, bp) and direct_receiver(ap, bp)
