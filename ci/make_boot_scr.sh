#!/bin/bash
# SPDX-License-Identifier: GPL-2.0+
#
# Generate boot.scr / u-boot.scr for a given carrier/platform identifier.
# Usage: make_boot_scr.sh <carrier> <mkimage> [output_dir]
#
# The carrier string is glob-matched to select the right boot recipe.
# mkimage path must be supplied (typically $KBUILD_OUTPUT/tools/mkimage).
# Output defaults to the current directory.

set -e

carrier="$1"
mkimage="$2"
output_dir="${3:-.}"

if [[ -z "$carrier" || -z "$mkimage" ]]; then
	echo "Usage: $0 <carrier> <mkimage> [output_dir]" >&2
	exit 1
fi

tmp_txt=$(mktemp --suffix=.txt)
trap 'rm -f "$tmp_txt"' EXIT

zynq_boot_scr() {
	cat > "$tmp_txt" <<-'SCRIPT'
	setenv bootargs earlycon clk_ignore_unused root=/dev/mmcblk0p2 rw rootwait
	load mmc 0:1 0x1f00000 system.dtb
	load mmc 0:1 0x2000000 uImage
	bootm 0x2000000 - 0x1f00000
	SCRIPT
	"$mkimage" -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "Boot script for Zynq" -d "$tmp_txt" "$output_dir/boot.scr"
}

zynqmp_boot_scr() {
	cat > "$tmp_txt" <<-'SCRIPT'
	setenv bootargs earlycon clk_ignore_unused root=/dev/mmcblk0p2 rw rootwait
	load mmc 0:1 0x40000000 system.dtb
	load mmc 0:1 0x18000000 Image
	booti 0x18000000 - 0x40000000
	SCRIPT
	"$mkimage" -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "Boot script for ZynqMP" -d "$tmp_txt" "$output_dir/boot.scr"
}

k26_boot_scr() {
	cat > "$tmp_txt" <<-'SCRIPT'
	setenv bootargs earlycon clk_ignore_unused root=/dev/mmcblk0p2 rw rootwait
	load mmc 0:1 0x40000000 system.dtb
	load mmc 0:1 0x18000000 Image
	booti 0x18000000 - 0x40000000
	SCRIPT
	"$mkimage" -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "Boot script for Kria K26" -d "$tmp_txt" "$output_dir/boot.scr"
}

jupiter_boot_scr() {
	cat > "$tmp_txt" <<-'SCRIPT'
	setenv bootargs earlycon clk_ignore_unused root=/dev/mmcblk0p2 rw rootwait
	load mmc 0:1 0x00100000 system.dtb
	load mmc 0:1 0x00200000 Image
	booti 0x00200000 - 0x00100000
	SCRIPT
	"$mkimage" -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "Boot script for Jupiter SDR" -d "$tmp_txt" "$output_dir/boot.scr"
}

no_bitfile_zynq_boot_scr() {
	cat > "$tmp_txt" <<-'SCRIPT'
	setenv bootargs earlycon clk_ignore_unused root=/dev/mmcblk0p2 rw rootwait
	load mmc 0:1 0x1f00000 system.dtb
	load mmc 0:1 0x2000000 uImage
	bootm 0x2000000 - 0x1f00000
	SCRIPT
	mkdir -p "$output_dir/no_bitfile"
	"$mkimage" -A arm -O linux -T script -C none -a 0 -e 0 \
		-n "Boot script for Zynq (no bitfile)" -d "$tmp_txt" "$output_dir/no_bitfile/boot.scr"
}

case "$carrier" in
	*jupiter_sdr*)                     jupiter_boot_scr ;;
	*zed*)                             zynq_boot_scr; no_bitfile_zynq_boot_scr ;;
	*zc702*|*zc706*)                   zynq_boot_scr ;;
	*zcu102*|*adrv2crr*)               zynqmp_boot_scr ;;
	*k26*|*kv260*)                     k26_boot_scr ;;
	*)
		echo "No boot.scr recipe for '$carrier', skipping"
		exit 0
		;;
esac
