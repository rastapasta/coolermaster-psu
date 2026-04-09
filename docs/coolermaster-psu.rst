.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver coolermaster-psu
==============================

Supported devices:

* Cooler Master Power Supplies

  Cooler Master X Silent Edge Platinum 850W

  Cooler Master X Silent MAX Platinum 1100W

  Cooler Master X Silent MAX Platinum 1300W

  Cooler Master X Mighty Platinum 2000

Author: Michael Straßburger <codepoet@cpan.org>

Description
-----------

This driver implements the sysfs interface for the Cooler Master X Silent /
X Mighty USB HID power supply family.

The device exposes one HID interface with device information, two temperature
channels, and a vendor-specific telemetry path that also appears to report AC
input and three DC output rails (12V, 3.3V, 5V). The driver enables sensor
reporting during initialization, consumes the live interrupt-IN telemetry path,
and falls back to direct temperature reads only when a temperature channel is
stale.

Sysfs entries
-------------

=======================	========================================================
temp1_input		Ambient temperature
temp2_input		Hotspot temperature
in0_input		AC input voltage
in1_input		12V rail voltage
in2_input		3.3V rail voltage
in3_input		5V rail voltage
curr1_input		AC input current
curr2_input		12V rail current
curr3_input		3.3V rail current
curr4_input		5V rail current
power1_input		AC input power
power2_input		Total output power
power3_input		12V rail power
power4_input		3.3V rail power
power5_input		5V rail power
=======================	========================================================

The exported values use standard hwmon units:

* temperatures in millidegree Celsius
* voltages in millivolts
* currents in milliamps
* powers in microwatts

Usage Notes
-----------

It is a USB HID device and is auto-detected. The driver uses standard HID
GET_REPORT and SET_REPORT requests to initialize telemetry, then consumes live
interrupt-IN reports for power updates.
