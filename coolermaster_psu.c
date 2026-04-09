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
 *   - output report 2, payload [2, 5]: disable sensor reporting
 *   - input report 3: power telemetry
 *   - input report 4: hotspot temperature
 *   - input report 5: ambient temperature
 *
 * Report 3 is delivered over the interrupt-IN path once reporting is enabled.
 * It reports AC input plus three output rails (12V, 3.3V, 5V). All values are
 * exported using standard hwmon units.
 */
#include <linux/device.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define DRIVER_NAME			"coolermaster-psu"
#define COOLERMASTER_PSU_VID				0x2516
#define COOLERMASTER_PSU_PID_X_SILENT_EDGE_850W		0x01a3
#define COOLERMASTER_PSU_PID_X_SILENT_MAX_1300W		0x01a5
#define COOLERMASTER_PSU_PID_X_SILENT_MAX_1100W		0x020c
#define COOLERMASTER_PSU_PID_X_MIGHTY_2000W		0x020e

#define COOLERMASTER_PSU_REPORT_INFO		0x01
#define COOLERMASTER_PSU_REPORT_CMD		0x02
#define COOLERMASTER_PSU_REPORT_POWER		0x03
#define COOLERMASTER_PSU_REPORT_HOTSPOT		0x04
#define COOLERMASTER_PSU_REPORT_AMBIENT		0x05

#define COOLERMASTER_PSU_CMD_ENABLE_SENSORS	0x04
#define COOLERMASTER_PSU_CMD_DISABLE_SENSORS	0x05

#define COOLERMASTER_PSU_INFO_LEN		7
#define COOLERMASTER_PSU_POWER_LEN		23
#define COOLERMASTER_PSU_TEMP_LEN		2
#define COOLERMASTER_PSU_ENABLE_WAIT_MS		3000
#define COOLERMASTER_PSU_POWER_STALE_MS		3000
#define COOLERMASTER_PSU_UPDATE_INTERVAL_MS	500

struct coolermaster_psu_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct mutex io_lock;
	spinlock_t data_lock;
	struct completion power_report_ready;
	unsigned long last_power_updated;
	unsigned long last_hotspot_updated;
	unsigned long last_ambient_updated;
	bool valid;
	bool enabled;
	bool have_live_power;
	bool have_hotspot;
	bool have_ambient;

	u8 report1[COOLERMASTER_PSU_INFO_LEN];
	u8 report3[COOLERMASTER_PSU_POWER_LEN];
	u8 report4[COOLERMASTER_PSU_TEMP_LEN];
	u8 report5[COOLERMASTER_PSU_TEMP_LEN];
	u8 cmd_buf[2];
	u8 io_temp_buf[COOLERMASTER_PSU_TEMP_LEN];

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

static int coolermaster_psu_get_report(struct coolermaster_psu_data *data,
				       u8 report_id, u8 *buf, size_t len)
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

static int coolermaster_psu_set_cmd(struct coolermaster_psu_data *data, u8 cmd)
{
	int ret;

	data->cmd_buf[0] = COOLERMASTER_PSU_REPORT_CMD;
	data->cmd_buf[1] = cmd;

	ret = hid_hw_raw_request(data->hdev, COOLERMASTER_PSU_REPORT_CMD,
				 data->cmd_buf, sizeof(data->cmd_buf),
				 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;
	if (ret != sizeof(data->cmd_buf))
		return -EIO;

	return 0;
}

static void coolermaster_psu_parse_power_report(struct coolermaster_psu_data *data)
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
}

static void coolermaster_psu_snapshot_status(struct coolermaster_psu_data *data,
					     bool *have_live_power,
					     bool *have_hotspot,
					     bool *have_ambient,
					     unsigned long *last_power_updated,
					     unsigned long *last_hotspot_updated,
					     unsigned long *last_ambient_updated)
{
	unsigned long flags;

	spin_lock_irqsave(&data->data_lock, flags);
	*have_live_power = data->have_live_power;
	*have_hotspot = data->have_hotspot;
	*have_ambient = data->have_ambient;
	*last_power_updated = data->last_power_updated;
	*last_hotspot_updated = data->last_hotspot_updated;
	*last_ambient_updated = data->last_ambient_updated;
	spin_unlock_irqrestore(&data->data_lock, flags);
}

static void coolermaster_psu_update_hotspot(struct coolermaster_psu_data *data,
					    const u8 *report)
{
	memcpy(data->report4, report, COOLERMASTER_PSU_TEMP_LEN);
	data->temp_hotspot_mc = data->report4[1] * 1000L;
	data->have_hotspot = true;
	data->last_hotspot_updated = jiffies;
}

static void coolermaster_psu_update_ambient(struct coolermaster_psu_data *data,
					    const u8 *report)
{
	memcpy(data->report5, report, COOLERMASTER_PSU_TEMP_LEN);
	data->temp_ambient_mc = data->report5[1] * 1000L;
	data->have_ambient = true;
	data->last_ambient_updated = jiffies;
}

static int coolermaster_psu_wait_power_report(struct coolermaster_psu_data *data,
					      unsigned int timeout_ms)
{
	if (!wait_for_completion_timeout(&data->power_report_ready,
					 msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;

	return 0;
}

static int coolermaster_psu_enable_reporting_locked(struct coolermaster_psu_data *data,
						    bool force_enable)
{
	int ret;

	if (data->enabled && !force_enable)
		return 0;

	ret = coolermaster_psu_get_report(data, COOLERMASTER_PSU_REPORT_INFO,
					  data->report1, sizeof(data->report1));
	if (ret)
		return ret;

	reinit_completion(&data->power_report_ready);
	ret = coolermaster_psu_set_cmd(data, COOLERMASTER_PSU_CMD_ENABLE_SENSORS);
	if (ret)
		return ret;

	data->enabled = true;

	return coolermaster_psu_wait_power_report(data,
						  COOLERMASTER_PSU_ENABLE_WAIT_MS);
}

static int coolermaster_psu_refresh_temps_locked(struct coolermaster_psu_data *data)
{
	int ret;
	bool need_ambient;
	bool need_hotspot;
	unsigned long flags;

	need_hotspot = !data->have_hotspot ||
		       time_is_before_jiffies(data->last_hotspot_updated +
					      msecs_to_jiffies(COOLERMASTER_PSU_UPDATE_INTERVAL_MS));
	need_ambient = !data->have_ambient ||
		       time_is_before_jiffies(data->last_ambient_updated +
					      msecs_to_jiffies(COOLERMASTER_PSU_UPDATE_INTERVAL_MS));

	if (need_hotspot) {
		ret = coolermaster_psu_get_report(data,
						  COOLERMASTER_PSU_REPORT_HOTSPOT,
						  data->io_temp_buf,
						  sizeof(data->io_temp_buf));
		if (ret)
			return ret;
		spin_lock_irqsave(&data->data_lock, flags);
		coolermaster_psu_update_hotspot(data, data->io_temp_buf);
		spin_unlock_irqrestore(&data->data_lock, flags);
	}

	if (need_ambient) {
		ret = coolermaster_psu_get_report(data,
						  COOLERMASTER_PSU_REPORT_AMBIENT,
						  data->io_temp_buf,
						  sizeof(data->io_temp_buf));
		if (ret)
			return ret;
		spin_lock_irqsave(&data->data_lock, flags);
		coolermaster_psu_update_ambient(data, data->io_temp_buf);
		spin_unlock_irqrestore(&data->data_lock, flags);
	}

	return 0;
}

static int coolermaster_psu_refresh_locked(struct coolermaster_psu_data *data)
{
	int ret;
	bool power_stale;
	bool temps_stale;
	bool have_live_power;
	bool have_hotspot;
	bool have_ambient;
	unsigned long last_power_updated;
	unsigned long last_hotspot_updated;
	unsigned long last_ambient_updated;

	coolermaster_psu_snapshot_status(data, &have_live_power, &have_hotspot,
					 &have_ambient, &last_power_updated,
					 &last_hotspot_updated,
					 &last_ambient_updated);

	power_stale = !have_live_power ||
		      time_is_before_jiffies(last_power_updated +
					     msecs_to_jiffies(COOLERMASTER_PSU_POWER_STALE_MS));
	if (!data->enabled || power_stale) {
		ret = coolermaster_psu_enable_reporting_locked(data, power_stale);
		if (ret)
			return ret;
	}

	coolermaster_psu_snapshot_status(data, &have_live_power, &have_hotspot,
					 &have_ambient, &last_power_updated,
					 &last_hotspot_updated,
					 &last_ambient_updated);

	temps_stale = !have_hotspot || !have_ambient ||
		      time_is_before_jiffies(last_hotspot_updated +
					     msecs_to_jiffies(COOLERMASTER_PSU_UPDATE_INTERVAL_MS)) ||
		      time_is_before_jiffies(last_ambient_updated +
					     msecs_to_jiffies(COOLERMASTER_PSU_UPDATE_INTERVAL_MS));
	if (temps_stale) {
		ret = coolermaster_psu_refresh_temps_locked(data);
		if (ret)
			return ret;
	}

	coolermaster_psu_snapshot_status(data, &have_live_power, &have_hotspot,
					 &have_ambient, &last_power_updated,
					 &last_hotspot_updated,
					 &last_ambient_updated);

	if (!have_live_power)
		return -ENODATA;

	data->valid = have_live_power && have_hotspot && have_ambient;

	return data->valid ? 0 : -ENODATA;
}

static int coolermaster_psu_refresh(struct coolermaster_psu_data *data)
{
	int ret;
	unsigned long flags;

	mutex_lock(&data->io_lock);
	ret = coolermaster_psu_refresh_locked(data);
	if (ret && data->enabled)
		ret = coolermaster_psu_enable_reporting_locked(data, true) ?:
		      coolermaster_psu_refresh_locked(data);
	mutex_unlock(&data->io_lock);

	if (ret)
		return ret;

	spin_lock_irqsave(&data->data_lock, flags);
	data->valid = data->have_live_power && data->have_hotspot &&
		      data->have_ambient;
	spin_unlock_irqrestore(&data->data_lock, flags);

	return ret;
}

static umode_t coolermaster_psu_is_visible(const void *drvdata,
					   enum hwmon_sensor_types type,
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

static int coolermaster_psu_read(struct device *dev,
				 enum hwmon_sensor_types type,
				 u32 attr, int channel, long *val)
{
	struct coolermaster_psu_data *data = dev_get_drvdata(dev);
	int ret;
	unsigned long flags;

	ret = coolermaster_psu_refresh(data);
	if (ret)
		return ret;

	spin_lock_irqsave(&data->data_lock, flags);
	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input) {
			*val = channel ? data->temp_hotspot_mc : data->temp_ambient_mc;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		}
		break;
	case hwmon_in:
		if (attr != hwmon_in_input)
			break;

		switch (channel) {
		case 0:
			*val = data->in_input_mv;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 1:
			*val = data->in_12v_mv;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 2:
			*val = data->in_3v3_mv;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 3:
			*val = data->in_5v_mv;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		default:
			break;
		}
		break;
	case hwmon_curr:
		if (attr != hwmon_curr_input)
			break;

		switch (channel) {
		case 0:
			*val = data->curr_input_ma;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 1:
			*val = data->curr_12v_ma;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 2:
			*val = data->curr_3v3_ma;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 3:
			*val = data->curr_5v_ma;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		default:
			break;
		}
		break;
	case hwmon_power:
		if (attr != hwmon_power_input)
			break;

		switch (channel) {
		case 0:
			*val = data->power_input_uw;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 1:
			*val = data->power_output_uw;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 2:
			*val = data->power_12v_uw;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 3:
			*val = data->power_3v3_uw;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		case 4:
			*val = data->power_5v_uw;
			spin_unlock_irqrestore(&data->data_lock, flags);
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&data->data_lock, flags);

	return -EOPNOTSUPP;
}

static int coolermaster_psu_read_string(struct device *dev,
					enum hwmon_sensor_types type,
					u32 attr, int channel,
					const char **str)
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

static const struct hwmon_ops coolermaster_psu_hwmon_ops = {
	.is_visible = coolermaster_psu_is_visible,
	.read = coolermaster_psu_read,
	.read_string = coolermaster_psu_read_string,
};

static const struct hwmon_channel_info * const coolermaster_psu_info[] = {
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

static const struct hwmon_chip_info coolermaster_psu_chip_info = {
	.ops = &coolermaster_psu_hwmon_ops,
	.info = coolermaster_psu_info,
};

static int coolermaster_psu_probe(struct hid_device *hdev,
				  const struct hid_device_id *id)
{
	struct coolermaster_psu_data *data;
	int ret;

	data = devm_kzalloc(&hdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->hdev = hdev;
	mutex_init(&data->io_lock);
	spin_lock_init(&data->data_lock);
	init_completion(&data->power_report_ready);
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

	hid_device_io_start(hdev);

	ret = coolermaster_psu_refresh(data);
	if (ret)
		hid_warn(hdev, "initial refresh failed: %d\n", ret);

	data->hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev,
							       "coolermaster_psu",
							       data,
							       &coolermaster_psu_chip_info,
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

static void coolermaster_psu_remove(struct hid_device *hdev)
{
	struct coolermaster_psu_data *data = hid_get_drvdata(hdev);

	if (data) {
		mutex_lock(&data->io_lock);
		if (data->enabled)
			coolermaster_psu_set_cmd(data,
						 COOLERMASTER_PSU_CMD_DISABLE_SENSORS);
		mutex_unlock(&data->io_lock);
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int coolermaster_psu_raw_event(struct hid_device *hdev,
				      struct hid_report *report, u8 *data, int size)
{
	struct coolermaster_psu_data *pdata = hid_get_drvdata(hdev);
	unsigned long flags;

	if (!pdata || size < 1)
		return 0;

	spin_lock_irqsave(&pdata->data_lock, flags);

	switch (data[0]) {
	case COOLERMASTER_PSU_REPORT_POWER:
		if (size >= COOLERMASTER_PSU_POWER_LEN) {
			memcpy(pdata->report3, data, COOLERMASTER_PSU_POWER_LEN);
			coolermaster_psu_parse_power_report(pdata);
			pdata->have_live_power = true;
			pdata->valid = pdata->have_hotspot && pdata->have_ambient;
			pdata->last_power_updated = jiffies;
		}
		break;
	case COOLERMASTER_PSU_REPORT_HOTSPOT:
		if (size >= COOLERMASTER_PSU_TEMP_LEN)
			coolermaster_psu_update_hotspot(pdata, data);
		break;
	case COOLERMASTER_PSU_REPORT_AMBIENT:
		if (size >= COOLERMASTER_PSU_TEMP_LEN)
			coolermaster_psu_update_ambient(pdata, data);
		break;
	default:
		break;
	}

	spin_unlock_irqrestore(&pdata->data_lock, flags);

	if (data[0] == COOLERMASTER_PSU_REPORT_POWER && size >= COOLERMASTER_PSU_POWER_LEN)
		complete(&pdata->power_report_ready);

	return 0;
}

static const struct hid_device_id coolermaster_psu_table[] = {
	{ HID_USB_DEVICE(COOLERMASTER_PSU_VID,
			 COOLERMASTER_PSU_PID_X_SILENT_EDGE_850W) },
	{ HID_USB_DEVICE(COOLERMASTER_PSU_VID,
			 COOLERMASTER_PSU_PID_X_SILENT_MAX_1300W) },
	{ HID_USB_DEVICE(COOLERMASTER_PSU_VID,
			 COOLERMASTER_PSU_PID_X_SILENT_MAX_1100W) },
	{ HID_USB_DEVICE(COOLERMASTER_PSU_VID,
			 COOLERMASTER_PSU_PID_X_MIGHTY_2000W) },
	{ }
};
MODULE_DEVICE_TABLE(hid, coolermaster_psu_table);

static struct hid_driver coolermaster_psu_driver = {
	.name = DRIVER_NAME,
	.id_table = coolermaster_psu_table,
	.probe = coolermaster_psu_probe,
	.remove = coolermaster_psu_remove,
	.raw_event = coolermaster_psu_raw_event,
};

static int __init coolermaster_psu_init(void)
{
	return hid_register_driver(&coolermaster_psu_driver);
}

static void __exit coolermaster_psu_exit(void)
{
	hid_unregister_driver(&coolermaster_psu_driver);
}

/* Initialize after the HID bus when built into the kernel. */
late_initcall(coolermaster_psu_init);
module_exit(coolermaster_psu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Straßburger <codepoet@cpan.org>");
MODULE_DESCRIPTION("Hwmon driver for Cooler Master USB HID PSUs");
