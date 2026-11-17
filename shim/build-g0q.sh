#!/bin/bash
# Galaxy S22+ (g0q) shim build on macOS: Apple clang for aarch64 ELF objects,
# Zig's ld.lld/objcopy for linking, AOSP mkbootimg for a header v4 BOOT image
# (the S22+ bootloader takes the DTB from the untouched stock vendor_boot).
#
# usage: shim/build-g0q.sh [payload.bin]
# needs: ../../tools-venv (ziglang, pyelftools), ../../tools-aosp/mkbootimg.py
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
PY="$ROOT/tools-venv/bin/python"
ZIG="$PY -m ziglang"
CC="clang -target aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-jump-tables -Wall -Wextra -D${G0Q:-G0Q_M2}"
SRCS="start.S shim.c console.c mmu.c fdt.c loader.c ufs.c g0q.c"
OUT=out-g0q
mkdir -p $OUT
objs=()
for f in $SRCS; do
	$CC $CFLAGS -c $f -o $OUT/${f%.*}.o
	objs+=("$OUT/${f%.*}.o")
done
$CC $CFLAGS -fno-builtin -c libmin.c -o $OUT/libmin.o
objs+=("$OUT/libmin.o")
# The image runs wherever the bootloader puts it with no relocation pass:
# only the header's image_size (start.o, linked at 0) may be absolute.
"$PY" -I - "${objs[@]:1}" <<'EOF'
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
bad = 0
for path in sys.argv[1:]:
    with open(path, "rb") as f:
        for sec in ELFFile(f).iter_sections():
            if isinstance(sec, RelocationSection):
                for r in sec.iter_relocations():
                    if r["r_info_type"] == 257:  # R_AARCH64_ABS64
                        print(f"error: ABS64 relocation in {path} {sec.name}", file=sys.stderr)
                        bad += 1
sys.exit(1 if bad else 0)
EOF
$ZIG ld.lld -static -T shim.ld --build-id=none -N --no-warn-rwx-segments "${objs[@]}" -o $OUT/shim.elf 2>/dev/null ||
	$ZIG ld.lld -static -T shim.ld --build-id=none -N "${objs[@]}" -o $OUT/shim.elf
$ZIG objcopy -O binary $OUT/shim.elf $OUT/Image
# Pad to image_size so .bss and the stacks exist as zeroed bytes.
size=$("$PY" -I -c "
import sys
from elftools.elf.elffile import ELFFile
e = ELFFile(open(sys.argv[1], 'rb'))
print(e.get_section_by_name('.symtab').get_symbol_by_name('_end')[0]['st_value'])" $OUT/shim.elf)
"$PY" -I -c "import os,sys; os.truncate(sys.argv[1], int(sys.argv[2]))" $OUT/Image "$size"
if [ -n "${1:-}" ]; then
	cp "$1" $OUT/ramdisk.bin
else
	(cd $OUT && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
# os_version / patch level copied from the stock S906E boot.img header.
PYTHONSAFEPATH= "$PY" "$ROOT/tools-aosp/mkbootimg.py" --header_version 4 --kernel $OUT/Image \
	--ramdisk $OUT/ramdisk.bin --os_version 12.0.0 --os_patch_level 2025-10 \
	--cmdline "" -o $OUT/shim-boot-g0q.img
cp $OUT/shim-boot-g0q.img $OUT/shim-boot-${G0Q:-G0Q_M2}.img
ls -l $OUT/Image $OUT/shim-boot-*.img
