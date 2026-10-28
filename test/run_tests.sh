#!/usr/bin/env bash
# Host tests: protocol, multiboot uploader, and the full GBA <-> module <-> GRBL simulation.
# Needs gcc/g++. If arm-none-eabi-gcc is on PATH the real GBA ROM image is built and used for
# the multiboot step; otherwise a dummy image is used.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/test/out}"
mkdir -p "$OUT/obj" "$OUT/shots"
SH="$ROOT/shared/linkbus_proto"
GC="$ROOT/gba/common"
GP="$ROOT/gba/pendant/src"
EL="$ROOT/esp/lib/LinkBus/src"
ES="$ROOT/esp/src"

echo "== protocol"
gcc -std=c99 -Wall -Wextra -I"$SH" "$ROOT/test/test_proto.c" "$SH/linkbus_proto.c" -o "$OUT/test_proto"
"$OUT/test_proto"

echo "== multiboot uploader"
gcc -std=c99 -Wall -Wextra -I"$EL" "$ROOT/test/test_multiboot.c" "$EL/lb_multiboot.c" -o "$OUT/test_multiboot"
"$OUT/test_multiboot"

echo "== GBA ROM (test link)"
ROM="$OUT/pendant.bin"
if command -v arm-none-eabi-gcc >/dev/null; then
  A="-mthumb -mthumb-interwork -mcpu=arm7tdmi -O2 -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections -fno-strict-aliasing"
  objs=()
  for f in "$GP/main.c" "$GP/app.c" "$GP/ui.c" "$GC/lb_gba.c" "$GC/gfx.c" "$GC/font8x8.c" "$SH/linkbus_proto.c"; do
    o="$OUT/obj/arm_$(basename "$f" .c).o"
    arm-none-eabi-gcc $A -I"$GC" -I"$SH" -I"$GP" -c "$f" -o "$o"
    objs+=("$o")
  done
  arm-none-eabi-as -mcpu=arm7tdmi "$ROOT/test/gba_link/crt0.s" -o "$OUT/obj/crt0.o"
  arm-none-eabi-gcc -mthumb -mthumb-interwork -mcpu=arm7tdmi -nostartfiles -T "$ROOT/test/gba_link/mb.ld" \
    "$OUT/obj/crt0.o" "${objs[@]}" -Wl,--gc-sections -lgcc -lc -o "$OUT/pendant.elf"
  arm-none-eabi-objcopy -O binary "$OUT/pendant.elf" "$ROM"
  arm-none-eabi-size "$OUT/pendant.elf"
else
  head -c 12000 /dev/urandom > "$ROM"; echo "(no ARM toolchain: dummy ROM image)"
fi

echo "== end-to-end simulation"
C="-DHOST_BUILD -O1 -g -Wall -Wno-unused-function"
for f in "$GP/app.c" "$GP/ui.c" "$GC/lb_gba.c" "$GC/gfx.c" "$GC/font8x8.c" "$SH/linkbus_proto.c" "$EL/lb_multiboot.c"; do
  gcc -std=gnu99 $C -I"$GC" -I"$SH" -I"$GP" -I"$EL" -c "$f" -o "$OUT/obj/host_$(basename "$f" .c).o"
done
for f in "$EL/LinkMaster.cpp" "$ES/Grbl.cpp" "$ES/CncModule.cpp" "$ROOT/test/host/test_e2e.cpp"; do
  g++ -std=gnu++17 $C -I"$ROOT/test/host" -I"$ES" -I"$EL" -I"$SH" -I"$GC" -I"$GP" -c "$f" -o "$OUT/obj/host_$(basename "$f" .cpp).o"
done
g++ "$OUT"/obj/host_*.o -o "$OUT/e2e"
SHOTS="$OUT/shots" "$OUT/e2e" "$ROM"
