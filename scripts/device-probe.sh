#!/bin/sh
# device-probe.sh — READ-ONLY. Inspects the Miyoo/OnionOS display, input, and
# available graphics libraries so we can choose the GUI rendering backend.
# Changes nothing on the device. Safe to run anytime.

echo "================ GAME TRACKER DEVICE PROBE ================"
echo "## kernel / os"
uname -a
[ -f /etc/os-release ] && cat /etc/os-release
[ -f /mnt/SDCARD/.tmp_update/onionVersion/version.txt ] && \
  echo "Onion: $(cat /mnt/SDCARD/.tmp_update/onionVersion/version.txt)"

echo
echo "## framebuffer (path A candidate)"
ls -la /dev/fb* 2>/dev/null || echo "  no /dev/fb* !"
for f in virtual_size bits_per_pixel stride name; do
  v=$(cat /sys/class/graphics/fb0/$f 2>/dev/null)
  [ -n "$v" ] && echo "  fb0/$f = $v"
done
command -v fbset >/dev/null 2>&1 && { echo "  -- fbset --"; fbset 2>/dev/null; } || echo "  (no fbset)"

echo
echo "## input devices (buttons)"
ls -la /dev/input/ 2>/dev/null
cat /proc/bus/input/devices 2>/dev/null | grep -iE "name|handlers" | head -20

echo
echo "## SDL2 present? (path B candidate)"
find / -name "libSDL2*" 2>/dev/null | head
echo "## vendor display libs (mi_gfx / mi_sys / mi_disp)"
find / \( -name "libmi_*" -o -name "libcam_os*" \) 2>/dev/null | head

echo
echo "## what an existing graphical app links against (RetroArch)"
RA=$(find /mnt/SDCARD -name "retroarch" -type f 2>/dev/null | head -1)
echo "  retroarch: $RA"
[ -n "$RA" ] && { command -v ldd >/dev/null 2>&1 && ldd "$RA" 2>/dev/null | head -25 || echo "  (no ldd; trying readelf)"; }
[ -n "$RA" ] && command -v readelf >/dev/null 2>&1 && readelf -d "$RA" 2>/dev/null | grep NEEDED | head -25

echo
echo "## terminal + libc"
command -v st && echo "  st: present"
ls -la /lib/libc.so* /lib/ld-* 2>/dev/null | head
echo "================ END PROBE ================"
