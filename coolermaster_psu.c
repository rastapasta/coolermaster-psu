// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * coolermaster-psu.c - Linux driver for Cooler Master PSUs with a HID sensors interface
 *
 * Copyright (C) 2026 Michael Straßburger <codepoet@cpan.org>
 *
 * The X Mighty Platinum 2000 exposes one HID interface with standard HID
 * GET_REPORT / SET_REPORT requests:
 *
 *   - input report 1: device / firmware information
 *   - output report 2, payload [2, 4]: enable sensor reporting
 *   - output report 2, payload [2, 3]: keepalive
 *   - output report 2, payload [2, 5]: disable sensor reporting
 *   - input report 3: power telemetry
 *   - input report 4: hotspot temperature
 *   - input report 5: ambient temperature
 *
 * Report 3 is 23 bytes long and reports AC input plus three output rails
 * (12V, 3.3V, 5V). All values are exported using standard hwmon units.
 */
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define DRIVER_NAME			"coolermaster-psu"
#define XMIGHTY_VID			0x2516
#define XMIGHTY_PID			0x020e

#define XMIGHTY_REPORT_INFO		0x01
#define XMIGHTY_REPORT_CMD		0x02
#define XMIGHTY_REPORT_POWER		0x03
#define XMIGHTY_REPORT_HOTSPOT		0x04
#define XMIGHTY_REPORT_AMBIENT		0x05

#define XMIGHTY_CMD_KEEPALIVE		0x03
#define XMIGHTY_CMD_ENABLE_SENSORS	0x04
#define XMIGHTY_CMD_DISABLE_SENSORS	0x05

#define XMIGHTY_INFO_LEN		7
#define XMIGHTY_POWER_LEN		23
#define XMIGHTY_TEMP_LEN		2
#define XMIGHTY_KEEPALIVE_RETRIES	8
#define XMIGHTY_ENABLE_SETTLE_MS	150
#define XMIGHTY_POLL_SETTLE_MS		50
#define XMIGHTY_UPDATE_INTERVAL_MS	500

struct xmighty_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct mutex lock;
	unsigned long last_updated;
	bool valid;
	bool enabled;

	u8 report1[XMIGHTY_INFO_LEN];
	u8 report3[XMIGHTY_POWER_LEN];
	u8 report4[XMIGHTY_TEMP_LEN];
	u8 report5[XMIGHTY_TEMP_LEN];

	long temp_ambient_mc;
	long temp_hotspot_mc;

	long in_input_mv;
	long in_12v_mv;
	long in_3v3_mv;
	long in_5v_mv;

	long curr_input_ma;
	long curr_12v_ma;
	long curr_3v3_ma;
	long curr_5v_ma;

	long power_input_uw;
	long power_output_uw;
	long power_12v_uw;
	long power_3v3_uw;
	long power_5v_uw;
};

static int xmighty_get_report(struct xmighty_data *data, u8 report_id, u8 *buf, size_t len)
{
	int ret;

	memset(buf, 0, len);
	buf[0] = report_id;

	ret = hid_hw_raw_request(data->hdev, report_id, buf, len,
				 HID_INPUT_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;

	return 0;
}

static int xmighty_set_cmd(struct xmighty_data *data, u8 cmd)
{
	u8 buf[2] = { XMIGHTY_REPORT_CMD, cmd };
	int ret;

	ret = hid_hw_raw_request(data->hdev, XMIGHTY_REPORT_CMD, buf, sizeof(buf),
				 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	return 0;
}

static void xmighty_parse_cache(struct xmighty_data *data)
{
	const u8 *r3 = data->report3;

	data->in_input_mv = get_unaligned_le16(&r3[1]) * 100;
	data->curr_input_ma = r3[3] * 100;
	data->power_input_uw = (long)get_unaligned_le16(&r3[4]) * 1000000L;

	data->power_output_uw = (long)get_unaligned_le16(&r3[6]) * 1000000L;

	data->in_12v_mv = r3[8] * 100;
	data->curr_12v_ma = get_unaligned_le16(&r3[9]) * 100;
	data->power_12v_uw = (long)get_unaligned_le16(&r3[11]) * 1000000L;

	data->in_3v3_mv = r3[13] * 100;
	data->curr_3v3_ma = get_unaligned_le16(&r3[14]) * 100;
	data->power_3v3_uw = (long)get_unaligned_le16(&r3[16]) * 1000000L;

	data->in_5v_mv = r3[18] * 100;
	data->curr_5v_ma = get_unaligned_le16(&r3[19]) * 100;
	data->power_5v_uw = (long)get_unaligned_le16(&r3[21]) * 1000000L;

	data->temp_hotspot_mc = data->report4[1] * 1000L;
	data->temp_ambient_mc = data->report5[1] * 1000L;
}

static int xmighty_try_refresh_locked(struct xmighty_data *data, bool force_enable)
{
	int ret, i;

	ret = xmighty_get_report(data, XMIGHTY_REPORT_INFO, data->report1, sizeof(data->report1));
	if (ret)
		return ret;

	if (!data->enabled || force_enable) {
		ret = xmighty_set_cmd(data, XMIGHTY_CMD_ENABLE_SENSORS);
		if (ret)
			return ret;
		data->enabled = true;
	}

	for (i = 0; i < XMIGHTY_KEEPALIVE_RETRIES; i++) {
		msleep(XMIGHTY_ENABLE_SETTLE_MS);

		ret = xmighty_set_cmd(data, XMIGHTY_CMD_KEEPALIVE);
		if (ret)
			return ret;

		ret = xmighty_get_report(data, XMIGHTY_REPORT_POWER, data->report3,
					 sizeof(data->report3));
		if (ret)
			return ret;

		if (memchr_inv(&data->report3[1], 0, sizeof(data->report3) - 1))
			break;
	}

	if (i == XMIGHTY_KEEPALIVE_RETRIES)
		return -EIO;

	msleep(XMIGHTY_POLL_SETTLE_MS);

	ret = xmighty_get_report(data, XMIGHTY_REPORT_HOTSPOT, data->report4,
				 sizeof(data->report4));
	if (ret)
		return ret;

	ret = xmighty_get_report(data, XMIGHTY_REPORT_AMBIENT, data->report5,
				 sizeof(data->report5));
	if (ret)
		return ret;

	xmighty_parse_cache(data);
	data->valid = true;
	data->last_updated = jiffies;

	return 0;
}

static int xmighty_refresh_locked(struct xmighty_data *data)
{
	if (data->valid &&
	    time_before(jiffies, data->last_updated +
				    msecs_to_jiffies(XMIGHTY_UPDATE_INTERVAL_MS)))
		return 0;

	return xmighty_try_refresh_locked(data, false) ?:
	       0;
}

static int xmighty_refresh(struct xmighty_data *data)
{
	int ret;

	mutex_lock(&data->lock);
	ret = xmighty_refresh_locked(data);
	if (ret && data->enabled)
		ret = xmighty_try_refresh_locked(data, true);
	mutex_unlock(&data->lock);

	return ret;
}

static umode_t xmighty_is_visible(const void *drvdata, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (channel < 2 &&
		    (attr == hwmon_temp_input || attr == hwmon_temp_label))
			return 0444;
		break;
	case hwmon_in:
		if (channel < 4 &&
		    (attr == hwmon_in_input || attr == hwmon_in_label))
			return 0444;
		break;
	case hwmon_curr:
		if (channel < 4 &&
		    (attr == hwmon_curr_input || attr == hwmon_curr_label))
			return 0444;
		break;
	case hwmon_power:
		if (channel < 5 &&
		    (attr == hwmon_power_input || attr == hwmon_power_label))
			return 0444;
		break;
	default:
		break;
	}

	return 0;
}

static int xmighty_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct xmighty_data *data = dev_get_drvdata(dev);
	int ret;

	ret = xmighty_refresh(data);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input) {
			*val = channel ? data->temp_hotspot_mc : data->temp_ambient_mc;
			return 0;
		}
		break;
	case hwmon_in:
		if (attr == hwmon_in_input) {
			switch (channel) {
			case 0: *val = data->in_input_mv; return 0;
			case 1: *val = data->in_12v_mv; return 0;
			case 2: *val = data->in_3v3_mv; return 0;
			case 3: *val = data->in_5v_mv; return 0;
			}
		}
		break;
	case hwmon_curr:
		if (attr == hwmon_curr_input) {
			switch (channel) {
			case 0: *val = data->curr_input_ma; return 0;
			case 1: *val = data->curr_12v_ma; return 0;
			case 2: *val = data->curr_3v3_ma; return 0;
			case 3: *val = data->curr_5v_ma; return 0;
			}
		}
		break;
	case hwmon_power:
		if (attr == hwmon_power_input) {
			switch (channel) {
			case 0: *val = data->power_input_uw; return 0;
			case 1: *val = data->power_output_uw; return 0;
			case 2: *val = data->power_12v_uw; return 0;
			case 3: *val = data->power_3v3_uw; return 0;
			case 4: *val = data->power_5v_uw; return 0;
			}
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int xmighty_read_string(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, const char **str)
{
	static const char * const temp_labels[] = { "ambient", "hotspot" };
	static const char * const in_labels[] = { "ac_input", "12V", "3.3V", "5V" };
	static const char * const curr_labels[] = { "ac_input", "12V", "3.3V", "5V" };
	static const char * const power_labels[] = { "input", "output", "12V", "3.3V", "5V" };

	if (attr == hwmon_temp_label && type == hwmon_temp && channel < ARRAY_SIZE(temp_labels)) {
		*str = temp_labels[channel];
		return 0;
	}
	if (attr == hwmon_in_label && type == hwmon_in && channel < ARRAY_SIZE(in_labels)) {
		*str = in_labels[channel];
		return 0;
	}
	if (attr == hwmon_curr_label && type == hwmon_curr &&
	    channel < ARRAY_SIZE(curr_labels)) {
		*str = curr_labels[channel];
		return 0;
	}
	if (attr == hwmon_power_label && type == hwmon_power &&
	    channel < ARRAY_SIZE(power_labels)) {
		*str = power_labels[channel];
		return 0;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_ops xmighty_hwmon_ops = {
	.is_visible = xmighty_is_visible,
	.read = xmighty_read,
	.read_string = xmighty_read_string,
};

static const struct hwmon_channel_info * const xmighty_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
				 HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL,
			       HWMON_I_INPUT | HWMON_I_LABEL,
			       HWMON_I_INPUT | HWMON_I_LABEL,
			       HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL,
				 HWMON_C_INPUT | HWMON_C_LABEL,
				 HWMON_C_INPUT | HWMON_C_LABEL,
				 HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL,
				  HWMON_P_INPUT | HWMON_P_LABEL,
				  HWMON_P_INPUT | HWMON_P_LABEL,
				  HWMON_P_INPUT | HWMON_P_LABEL,
				  HWMON_P_INPUT | HWMON_P_LABEL),
	NULL,
};

static const struct hwmon_chip_info xmighty_chip_info = {
	.ops = &xmighty_hwmon_ops,
	.info = xmighty_info,
};

static int xmighty_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct xmighty_data *data;
	int ret;

	data = devm_kzalloc(&hdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->hdev = hdev;
	mutex_init(&data->lock);
	hid_set_drvdata(hdev, data);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto err_stop;

	ret = xmighty_refresh(data);
	if (ret)
		hid_warn(hdev, "initial refresh failed: %d\n", ret);

	data->hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev,
							       "coolermaster_psu",
							       data,
							       &xmighty_chip_info,
							       NULL);
	if (IS_ERR(data->hwmon_dev)) {
		ret = PTR_ERR(data->hwmon_dev);
		goto err_close;
	}

	hid_info(hdev, "Cooler Master PSU hwmon driver bound\n");
	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void xmighty_remove(struct hid_device *hdev)
{
	struct xmighty_data *data = hid_get_drvdata(hdev);

	if (data) {
		mutex_lock(&data->lock);
		if (data->enabled)
			xmighty_set_cmd(data, XMIGHTY_CMD_DISABLE_SENSORS);
		mutex_unlock(&data->lock);
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id xmighty_table[] = {
	{ HID_USB_DEVICE(XMIGHTY_VID, XMIGHTY_PID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, xmighty_table);

static struct hid_driver xmighty_driver = {
	.name = DRIVER_NAME,
	.id_table = xmighty_table,
	.probe = xmighty_probe,
	.remove = xmighty_remove,
};

static int __init xmighty_init(void)
{
	return hid_register_driver(&xmighty_driver);
}

static void __exit xmighty_exit(void)
{
	hid_unregister_driver(&xmighty_driver);
}

/* Initialize after the HID bus when built into the kernel. */
late_initcall(xmighty_init);
module_exit(xmighty_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Straßburger <codepoet@cpan.org>");
MODULE_DESCRIPTION("Hwmon driver for Cooler Master X Mighty Platinum 2000 PSU");
