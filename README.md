# coolermaster-psu

Linux HID + hwmon driver for the Cooler Master USB PSU family.

This repository contains:

- an out-of-tree kernel module
- DKMS packaging metadata
- a helper script and systemd unit for binding the driver
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
3. `SET_REPORT` output report `2`, payload `[2, 3]` as keepalive
4. `GET_REPORT` input reports `3`, `4`, `5` for power + temperatures

Exposed telemetry:

- ambient temperature
- hotspot temperature
- AC input voltage / current / power
- 12V rail voltage / current / power
- 3.3V rail voltage / current / power
- 5V rail voltage / current / power
- total output power

## hwmon units

Values follow standard `hwmon` units, same convention used by in-tree drivers
such as `corsair-psu`:

- `temp*_input`: millidegree Celsius
- `in*_input`: millivolts
- `curr*_input`: milliamps
- `power*_input`: microwatts

Examples:

- `41000` = `41.000 C`
- `12300` = `12.300 V`
- `500` = `0.500 A`
- `119000000` = `119.000000 W`

## Repository layout

- `coolermaster_psu.c`: out-of-tree module source
- `Makefile`: out-of-tree module build
- `dkms.conf`: DKMS metadata
- `bind-coolermaster-psu.sh`: helper to load and bind the driver
- `coolermaster-psu.service`: example systemd unit
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

## Secure Boot

If Secure Boot is enabled, sign the module with an enrolled key or use DKMS'
MOK workflow. On the test host, the DKMS-generated MOK key had to be enrolled
before the module could load.

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

Use the helper script if you want this automated:

```sh
sh ./bind-coolermaster-psu.sh
```

## lm-sensors example

Install:

```sh
cp sensors.d/coolermaster-psu.conf /etc/sensors.d/
```
