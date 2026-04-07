#!/bin/sh
set -eu

TARGET_VID="2516"
TARGET_PID="020E"

if ! lsmod | grep -q '^coolermaster_psu '; then
	modprobe coolermaster_psu 2>/dev/null || insmod "$(dirname "$0")/coolermaster_psu.ko"
fi

for dev in /sys/bus/hid/devices/*; do
	[ -f "$dev/uevent" ] || continue
	if ! grep -q "HID_ID=0003:0000${TARGET_VID}:0000${TARGET_PID}" "$dev/uevent"; then
		continue
	fi

	name="$(basename "$dev")"
	current_driver=""

	if [ -L "$dev/driver" ]; then
		current_driver="$(basename "$(readlink -f "$dev/driver")")"
	fi

	if [ "$current_driver" = "coolermaster_psu" ]; then
		continue
	fi

	if [ -e "/sys/bus/hid/drivers/hid-generic/$name" ]; then
		echo "$name" > /sys/bus/hid/drivers/hid-generic/unbind
	fi

	if [ -e "/sys/bus/hid/drivers/coolermaster_psu/bind" ] &&
	   [ ! -e "/sys/bus/hid/drivers/coolermaster_psu/$name" ]; then
		echo "$name" > /sys/bus/hid/drivers/coolermaster_psu/bind
	fi
done

exit 0
