#!/usr/bin/env bash

# Builds debugging firmware for a single device using the build system.
# Updates indexes, converters, quirks, extensions and supported devices list.

# Estimated runtime: 5 seconds

# Requires dependencies, sdk and toolchain.
#   Get them by runnnig make_install.sh.

# Debug flag enables prints. To view them:
#   connect a serial monitor to the UART TX pin.

# Suggestion: Copy this script to a gitignored folder
#   and update it for your device.

set -e                                           # Exit on error.
cd "$(dirname "$(dirname "$(realpath "$0")")")"  # Go to project root.

DEVICE=SWITCH_ZEMISMART_2_TS0012  # Change this to your device

# Check if device exists in database
if ! yq -e ".${DEVICE}" device_db.yaml >/dev/null 2>&1; then
    echo "Error: Device '$DEVICE' not found in device_db.yaml"
    echo "Available devices:"
    yq 'keys[]' device_db.yaml | head -10
    echo "... (run 'yq keys device_db.yaml' to see all)"
    exit 1
fi

# Get device info from database
MCU=$(yq -r ".${DEVICE}.mcu" device_db.yaml)
PLATFORM=$(if [[ "$MCU" == "TLSR8258" ]]; then echo "telink"; else echo "silabs"; fi)

echo "Building debug firmware for device: $DEVICE (MCU: $MCU, Platform: $PLATFORM)"

# Get device info from database
MCU=$(yq -r ".${DEVICE}.mcu" device_db.yaml)
TYPE=$(yq -r ".${DEVICE}.device_type" device_db.yaml) # Get the actual type (end_device)
PLATFORM=$(if [[ "$MCU" == "TLSR8258" ]]; then echo "telink"; else echo "silabs"; fi)

echo "Building debug firmware for device: $DEVICE (MCU: $MCU, Type: $TYPE)"

# Build only the version defined in the database
echo "=== Building $TYPE version with debug ==="
BOARD=$DEVICE DEVICE_TYPE=$TYPE DEBUG=1 make board/build-firmware

# Updated check to look in the correct directory
if [[ "$TYPE" == "end_device" ]]; then
    ls -l bin/end_device/${DEVICE}_END_DEVICE/ 2>/dev/null || echo "No end_device files found"
else
    ls -l bin/router/$DEVICE/ 2>/dev/null || echo "No router files found"
fi

echo "=== Updating integration files ==="
make tools/update_converters
make tools/update_zha_quirk
make tools/update_homed_extension
make tools/update_supported_devices

echo "=== Build complete ==="
echo "Debug firmware built for $DEVICE"
echo "Connect UART to view debug output on TX pin"