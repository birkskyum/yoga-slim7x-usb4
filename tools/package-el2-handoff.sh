#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# LOCAL ONLY. Requires the owner's firmware and previously verified boot assets.
# No device writes, mounts, firmware flashing, NVRAM edits or network access.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
kernel=${1:?kernel source}
build=${2:?matching completed handoff build}
qeb=${3:?matching qebspil EFI build}
firmware=${4:?private firmware directory}
legacy=${5:?previously verified EFI shell, SLBounce and launch payload directory}
output=${6:?new output directory}
[[ ! -e "$output" ]] || { echo 'Refusing existing output' >&2; exit 1; }
for name in kernel build qeb firmware legacy; do
    printf -v "$name" '%s' "$(realpath "${!name}")"
done
check_hash() { [[ $(sha256sum "$1" | cut -d' ' -f1) == "$2" ]]; }
check_hash "$legacy/EFI/BOOT/BOOTAA64.EFI" 1569b6db4e391c3c59194aa3319a3945efb800fb25349eb9d36ff3d258517ea6
check_hash "$legacy/slbounce.efi" cc1b62e8bafeac98b80c99397405f3290af965801a4df0d2416ef120a2fbf6bd
check_hash "$legacy/tcblaunch.exe" 5dfcd0253b6ee99499ab33cac221e8a9cea47f3fdf6d4e11de9a9f3c4770d03d
grep -qx CONFIG_USB4_X1_ADSP_HANDOFF=y "$build/.config"
grep -qx '# CONFIG_USB4_X1_EL2_TEST is not set' "$build/.config"
grep -qx CONFIG_QCOM_Q6V5_PAS=m "$build/.config"
grep -qx CONFIG_QCOM_TZMEM_MODE_GENERIC=y "$build/.config"
grep -qx CONFIG_ARM_SMMU_V3=y "$build/.config"
test -s "$build/arch/arm64/boot/Image"
mkdir -p "$output"
output=$(realpath "$output")
python3 "$project/tools/check-el2-handoff.py" --kernel "$kernel" \
    --firmware-dir "$firmware" --output "$output/handoff.dtb"
cd "$output"
version=$(cat "$build/include/config/kernel.release")
[[ "$version" == *-yoga-x1-el2-handoff ]]
mkdir -p ramroot/{bin,sbin,dev,proc,sys,run,tmp,lib/modules}
install -m755 /bin/busybox ramroot/bin/busybox
ln -s busybox ramroot/bin/sh
install -m755 "$project/snapshot/runtime/init-el2-handoff" ramroot/init
install -m755 "$project/snapshot/runtime/check-adsp" ramroot/bin/check-adsp
moddir=ramroot/lib/modules/$version
mkdir -p "$moddir/extra"
for mod in drivers/remoteproc/qcom_q6v5 drivers/remoteproc/qcom_q6v5_pas \
           drivers/remoteproc/qcom_common drivers/remoteproc/qcom_pil_info \
           drivers/soc/qcom/mdt_loader; do
    install -m644 "$build/$mod.ko" "$moddir/extra/"
    printf 'extra/%s.ko\n' "${mod##*/}" >> "$moddir/modules.order"
done
cp "$build/modules.builtin" "$build/modules.builtin.modinfo" "$moddir/"
depmod -b ramroot "$version"
modprobe -d ramroot -S "$version" --show-depends qcom_q6v5_pas > module-dependencies.txt
! grep -E 'nvme|yoga_x1_mcu_probe|ps883|r8152' module-dependencies.txt
(cd ramroot && find . -print0 | LC_ALL=C sort -z | cpio --null -o --format=newc --owner=0:0) | gzip -n > initramfs.cpio.gz
gzip -dc initramfs.cpio.gz | cpio -it > initramfs.files
! grep -E 'lib/firmware|nvme.ko|probe-usb4|read-lacie|yoga_x1_mcu_probe.ko' initramfs.files
args='rdinit=/init console=tty0 earlycon=efifb keep_bootcon clk_ignore_unused pd_ignore_unused regulator_ignore_unused consoleblank=0 efi=noruntime arm64.nopauth loglevel=5 panic=0 iommu.passthrough=0 iommu.strict=1'
/usr/lib/systemd/ukify build --config=/dev/null --efi-arch=aa64 \
    --stub=/usr/lib/systemd/boot/efi/linuxaa64.efi.stub --linux="$build/arch/arm64/boot/Image" \
    --initrd=initramfs.cpio.gz --devicetree=handoff.dtb --cmdline="$args" \
    --os-release=@"$project/uefi/os-release" --uname="$version" --output=handoff.efi
mkdir verified-sections payload
objcopy --dump-section .dtb=verified-sections/dtb --dump-section .linux=verified-sections/linux \
    --dump-section .initrd=verified-sections/initrd --dump-section .cmdline=verified-sections/cmdline \
    handoff.efi verified-sections/copy.efi
cmp handoff.dtb verified-sections/dtb
cmp "$build/arch/arm64/boot/Image" verified-sections/linux
cmp initramfs.cpio.gz verified-sections/initrd
printf '%s' "$args" | cmp verified-sections/cmdline -
mkdir -p payload/EFI/BOOT payload/firmware/qcom/x1e80100/LENOVO/83ED
cp "$legacy/EFI/BOOT/BOOTAA64.EFI" payload/EFI/BOOT/
cp "$legacy/slbounce.efi" "$legacy/tcblaunch.exe" payload/
cp "$qeb" payload/qebspilaa64.efi
cp "$firmware/qcadsp8380.mbn" "$firmware/adsp_dtbs.elf" payload/firmware/qcom/x1e80100/LENOVO/83ED/
cp "$project/uefi/startup.nsh" "$project/uefi/handoff.nsh" handoff.efi payload/
printf 'EL2 ADSP service-only checkpoint. No USB4/storage test.\n' > payload/YOGA-EL2-HANDOFF.marker
truncate -s 128M yoga-el2-handoff.img
printf 'label: gpt\nstart=2048, size=258048, type=U\n' | sfdisk yoga-el2-handoff.img
fat_image=yoga-el2-handoff.img@@1048576
mformat -i "$fat_image" -T 258048 -c 1 -F -v YOGAEL2 ::
mcopy -s -i "$fat_image" payload/* ::/
while IFS= read -r -d '' file; do
    relative=${file#payload/}
    mtype -i "$fat_image" "::/$relative" | cmp "$file" -
done < <(find payload -type f -print0)
sfdisk --verify yoga-el2-handoff.img
sha256sum "$build/.config" "$build/arch/arm64/boot/Image" handoff.dtb \
    initramfs.cpio.gz handoff.efi "$qeb" yoga-el2-handoff.img > SHA256SUMS
cat SHA256SUMS
echo 'LOCAL CANDIDATE ONLY. No disk was written. Requires reviewed cold-boot handoff test.'
