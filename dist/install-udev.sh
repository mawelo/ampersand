#!/usr/bin/env bash
# Installs the udev rule so /dev/hidrawN for the Quad Cortex is usable
# without root. Run once: sudo ./install-udev.sh
set -euo pipefail
d=$(cd "$(dirname "$0")" && pwd)
install -m 0644 "$d/99-neural-quad-cortex.rules" /etc/udev/rules.d/99-neural-quad-cortex.rules
udevadm control --reload-rules && udevadm trigger
echo 'udev rule installed. Replug the Quad Cortex, then run the AppImage.'
