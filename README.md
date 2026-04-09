# coolermaster-psu

Linux HID + hwmon driver for the Cooler Master USB PSU family.

This repository contains:

- an out-of-tree kernel module
- DKMS packaging metadata
- an `lm-sensors` config example

## Current status

Validated on:

- Proxmox VE kernel `6.17.13-2-pve`
- device: `Cooler Master Technology Inc. X Mighty Platinum 2000`

Confirmed supported model family:

- Cooler Master X Silent Edge Platinum 850W (`2516:01a3`)
- Cooler Master X Silent MAX Platinum 1300W (`2516:01a5`)
- Cooler Master X Silent MAX Platinum 1100W (`2516:020c`)
- Cooler Master X Mighty Platinum 2000 (`2516:020e`)

Recovered protocol flow:

1. `GET_REPORT` input report `1` for device / firmware info
2. `SET_REPORT` output report `2`, payload `[2, 4]` to enable sensor reporting
3. interrupt-IN `report 3` for live power telemetry
4. interrupt-IN `report 4` / `5` for live hotspot and ambient temperatures
5. `GET_REPORT` input `4` / `5` as fallback if a temperature channel is stale

Exposed telemetry:

- ambient temperature
- hotspot temperature
- AC input voltage / current / power
- 12V rail voltage / current / power
- 3.3V rail voltage / current / power
- 5V rail voltage / current / power

## hwmon units

Values follow standard `hwmon` units, same convention used by in-tree drivers
such as `corsair-psu`:

- `temp*_input`: millidegree Celsius

Examples:

- `41000` = `41.000 C`

## Repository layout

- `coolermaster_psu.c`: out-of-tree module source
- `Makefile`: out-of-tree module build
- `dkms.conf`: DKMS metadata
- `sensors.d/coolermaster-psu.conf`: `lm-sensors` labels
- `docs/coolermaster-psu.rst`: latest upstream-targeted hwmon documentation text

## Local build

```sh
make
```

## DKMS install

Example flow on Debian:

```sh
apt install dkms linux-headers-$(uname -r)
cp -a coolermaster-psu /usr/src/coolermaster-psu-0.1.0
dkms add -m coolermaster-psu -v 0.1.0
dkms build -m coolermaster-psu -v 0.1.0
dkms install -m coolermaster-psu -v 0.1.0
modprobe coolermaster_psu
```

Once installed, the module can bind like any other HID driver through its
modalias. No helper script, `systemd` unit, or `/etc/modules-load.d/` entry is
required.

## Secure Boot

If Secure Boot is enabled, sign the module with an enrolled key or use DKMS'
MOK workflow. On

## Bind / unbind

Manual bind test:

```sh
echo 0003:2516:020E.000X > /sys/bus/hid/drivers/hid-generic/unbind
modprobe coolermaster_psu
echo 0003:2516:020E.000X > /sys/bus/hid/drivers/coolermaster_psu/bind
```

Unload and restore generic HID:

```sh
echo 0003:2516:020E.000X > /sys/bus/hid/drivers/coolermaster_psu/unbind
modprobe -r coolermaster_psu
echo 0003:2516:020E.000X > /sys/bus/hid/drivers/hid-generic/bind
```

## lm-sensors example

Install:

```sh
cp sensors.d/coolermaster-psu.conf /etc/sensors.d/
```
