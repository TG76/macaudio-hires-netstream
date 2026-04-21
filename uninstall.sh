#!/bin/bash
set -e

DRIVER_NAME="FocalHiRes"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"

echo "=== Removing ${DRIVER_NAME}.driver ==="
sudo rm -rf "${HAL_DIR}/${DRIVER_NAME}.driver"

echo "=== Restarting coreaudiod ==="
sudo launchctl stop com.apple.audio.coreaudiod
sleep 1
sudo launchctl start com.apple.audio.coreaudiod

echo "=== Done! ==="
