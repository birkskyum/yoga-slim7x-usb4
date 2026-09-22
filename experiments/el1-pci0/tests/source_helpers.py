# SPDX-License-Identifier: GPL-2.0-only
"""Extract actual C functions for the bounded IRQ/MMIO mock tests."""
import re


def function(text, name):
    match = re.search(r'^(?:static )?(?:inline )?(?:int|bool|void|u64|ssize_t|struct x1_host \*|struct tb_port \*|struct tb_switch \*)\s*' + name + r'\([^;{]*\)\s*\{', text, re.M)
    if not match:
        raise ValueError(name)
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]
