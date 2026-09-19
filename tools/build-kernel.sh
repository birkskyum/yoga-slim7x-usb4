#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
kernel="$project/build-output/kernel"
output="$project/build-output/compile"
module="$project/build-output/module"
[[ -f "$kernel/Makefile" ]] || { echo 'Run tools/review.py prepare first.' >&2; exit 1; }
[[ ! -e "$output" && ! -e "$module" ]] || { echo 'Build output already exists; preserve it and choose a new checkout.' >&2; exit 1; }
mkdir -p "$output"
cp "$project/reproduce/v38.config" "$output/.config"
cp -R "$project/snapshot/module" "$module"
"$kernel/scripts/config" --file "$output/.config" --set-str LOCALVERSION '-yoga-x1-v38-public-failclosed'
make -C "$kernel" O="$output" ARCH=arm64 olddefconfig
make -C "$kernel" O="$output" ARCH=arm64 -j"${JOBS:-6}" Image qcom/x1e80100-lenovo-yoga-slim7x.dtb
make -C "$kernel" O="$output" ARCH=arm64 -j"${JOBS:-6}" \
  drivers/remoteproc/qcom_q6v5.ko drivers/remoteproc/qcom_q6v5_pas.ko \
  drivers/remoteproc/qcom_common.ko drivers/remoteproc/qcom_pil_info.ko \
  drivers/soc/qcom/mdt_loader.ko drivers/usb/typec/mux/ps883x.ko drivers/nvme/host/nvme.ko
cp "$output/vmlinux.symvers" "$output/Module.symvers"
make -C "$kernel" O="$output" ARCH=arm64 M="$module" W=1 -j"${JOBS:-6}" modules
echo 'Built public fail-closed sources. No firmware packaged, nothing installed or flashed.'
