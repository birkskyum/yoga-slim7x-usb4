#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Run in an aarch64 Linux build environment with git, make, gcc and binutils.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
destination=${1:?Choose a new local output directory}
[[ ! -e "$destination" ]] || { echo 'Refusing existing output' >&2; exit 1; }
git clone --no-checkout https://github.com/stephan-gh/qebspil.git "$destination"
git -C "$destination" checkout --detach 8e4d9e676a3b3afe136cda9b953a2139ff1a32d0
git -C "$destination" submodule update --init --recursive
[[ $(git -C "$destination/external/dtc" rev-parse HEAD) == 2d10aa2afe35527728db30b35ec491ecb6959e5c ]]
[[ $(git -C "$destination/external/gnu-efi" rev-parse HEAD) == 1fee8ab566ce91b9cbab9f2c85db96566d79063b ]]
python3 "$project/tools/check-qebspil.py" --source "$destination"
git -C "$destination" apply --check "$project/uefi/qebspil-yoga.patch"
git -C "$destination" apply "$project/uefi/qebspil-yoga.patch"
python3 "$project/tools/check-qebspil.py" --source "$destination" --patched
make -C "$destination" -j"${JOBS:-4}" QEBSPIL_ALWAYS_START=0
sha256sum "$destination/out/qebspilaa64.efi"
echo 'Built only. This does not approve a hardware boot or install anything.'
