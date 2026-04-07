.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver coolermaster-psu
==============================

Supported devices:

* Cooler Master Power Supplies

  Cooler Master X Mighty Platinum 2000

Author: Michael Straßburger

Description
-----------

This driver implements the sysfs interface for the Cooler Master X Mighty
Platinum 2000 power supply with a USB HID protocol interface.

The device exposes one HID interface with device information, two temperature
channels, AC input telemetry and three DC output rails (12V, 3.3V, 5V). The
driver enables sensor reporting during initialization and keeps the device alive
with a periodic software poll.

Sysfs entries
-------------

=======================	========================================================
curr1_input		Total AC input current
curr2_input		Current on the 12V rail
curr3_input		Current on the 3.3V rail
curr4_input		Current on the 5V rail
in0_input		AC input voltage
in1_input		Voltage on the 12V rail
in2_input		Voltage on the 3.3V rail
in3_input		Voltage on the 5V rail
power1_input		Total AC input power
power2_input		Total DC output power
power3_input		Power on the 12V rail
power4_input		Power on the 3.3V rail
power5_input		Power on the 5V rail
temp1_input		Ambient temperature
temp2_input		Hotspot temperature
=======================	========================================================

All values use standard hwmon units:

* voltages in millivolts
* currents in milliamps
* powers in microwatts
* temperatures in millidegree Celsius

Usage Notes
-----------

It is a USB HID device and is auto-detected. The driver uses standard HID
GET_REPORT and SET_REPORT requests to retrieve telemetry and to keep reporting
enabled.
