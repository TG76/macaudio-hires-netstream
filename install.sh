#!/bin/bash
set -e

DRIVER_NAME="FocalHiRes"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"
BUILD_DIR="build"

echo "=== Building ${DRIVER_NAME} ==="
cmake -B "${BUILD_DIR}" -S . -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j$(sysctl -n hw.ncpu)

echo ""
echo "=== Installing ${DRIVER_NAME}.driver ==="
# WICHTIG: Zielordner vorher entfernen, sonst kopiert cp -R INS existierende
# Verzeichnis hinein statt es zu ersetzen -> verschachteltes Bundle, Signatur
# ungültig, coreaudiod lehnt Plug-in ab.
sudo rm -rf "${HAL_DIR}/${DRIVER_NAME}.driver"
sudo cp -R "${BUILD_DIR}/${DRIVER_NAME}.driver" "${HAL_DIR}/"

# Ad-hoc re-sign NACH allen File-Ops (chown/cp/etc. invalidieren Signatur).
# --deep signiert auch das Binary in Contents/MacOS/.
sudo codesign --force --sign - --deep "${HAL_DIR}/${DRIVER_NAME}.driver"
sudo codesign --verify --verbose "${HAL_DIR}/${DRIVER_NAME}.driver"

echo ""
echo "=== Restarting coreaudiod ==="
sudo killall coreaudiod

echo ""
echo "=== Done! ==="
echo "\"Focal HiRes\" should now appear in your sound output menu."
