/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/zephyr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/settings/settings.h>
#include <bluetooth/services/fast_pair.h>

#include <dk_buttons_and_leds.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(fp_sample, LOG_LEVEL_INF);

#include "bt_adv_helper.h"
#include "hids_helper.h"

#define RUN_STATUS_LED						DK_LED1
#define CON_STATUS_LED						DK_LED2
#define FP_ADV_MODE_STATUS_LED					DK_LED3

#define FP_ADV_MODE_BUTTON_MASK					DK_BTN1_MSK
#define VOLUME_UP_BUTTON_MASK					DK_BTN2_MSK
#define VOLUME_DOWN_BUTTON_MASK					DK_BTN4_MSK

#define RUN_LED_BLINK_INTERVAL_MS				1000
#define FP_ADV_MODE_SHOW_UI_INDICATION_LED_BLINK_INTERVAL_MS	500
#define FP_ADV_MODE_HIDE_UI_INDICATION_LED_BLINK_INTERVAL_MS	1500

static enum bt_fast_pair_adv_mode fp_adv_mode = BT_FAST_PAIR_ADV_MODE_DISCOVERABLE;
static struct bt_conn *peer;

static struct k_work bt_adv_restart;
static struct k_work_delayable fp_adv_mode_status_led_handle;

static void advertising_start(void)
{
	int err = bt_adv_helper_adv_start(fp_adv_mode);

	if (!err) {
		if (fp_adv_mode == BT_FAST_PAIR_ADV_MODE_DISCOVERABLE) {
			LOG_INF("Discoverable advertising started");
		} else {
			LOG_INF("Non-discoverable advertising started, %s UI indication enabled",
				(fp_adv_mode == BT_FAST_PAIR_ADV_MODE_NOT_DISCOVERABLE_SHOW_UI_IND)
					? "show" : "hide");
		}
	} else {
		LOG_ERR("Advertising failed to start (err %d)", err);
	}
}

static void bt_adv_restart_fn(struct k_work *w)
{
	advertising_start();
}

static void fp_adv_mode_status_led_handle_fn(struct k_work *w)
{
	static bool led_on = true;

	switch (fp_adv_mode) {
	case BT_FAST_PAIR_ADV_MODE_DISCOVERABLE:
		dk_set_led_on(FP_ADV_MODE_STATUS_LED);
		break;

	case BT_FAST_PAIR_ADV_MODE_NOT_DISCOVERABLE_SHOW_UI_IND:
		dk_set_led(FP_ADV_MODE_STATUS_LED, led_on);
		led_on = !led_on;
		k_work_reschedule(&fp_adv_mode_status_led_handle,
				  K_MSEC(FP_ADV_MODE_SHOW_UI_INDICATION_LED_BLINK_INTERVAL_MS));
		break;

	case BT_FAST_PAIR_ADV_MODE_NOT_DISCOVERABLE_HIDE_UI_IND:
		dk_set_led(FP_ADV_MODE_STATUS_LED, led_on);
		led_on = !led_on;
		k_work_reschedule(&fp_adv_mode_status_led_handle,
				  K_MSEC(FP_ADV_MODE_HIDE_UI_INDICATION_LED_BLINK_INTERVAL_MS));
		break;

	default:
		__ASSERT_NO_MSG(false);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	/* Multiple simultaneous connections are not supported by the sample. */
	__ASSERT_NO_MSG(!peer);

	if (err) {
		LOG_WRN("Connection failed (err %u)", err);
		k_work_submit(&bt_adv_restart);
		return;
	}

	LOG_INF("Connected");

	dk_set_led_on(CON_STATUS_LED);
	peer = conn;
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason %u)", reason);

	dk_set_led_off(CON_STATUS_LED);
	peer = NULL;

	k_work_submit(&bt_adv_restart);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", log_strdup(addr), level);
	} else {
		LOG_WRN("Security failed: %s level %u err %d", log_strdup(addr), level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.security_changed = security_changed,
};

static const char *volume_change_to_str(enum hids_helper_volume_change volume_change)
{
	const char *res = NULL;

	switch (volume_change) {
	case HIDS_HELPER_VOLUME_CHANGE_DOWN:
		res = "Decrease";
		break;

	case HIDS_HELPER_VOLUME_CHANGE_NONE:
		break;

	case HIDS_HELPER_VOLUME_CHANGE_UP:
		res = "Increase";
		break;

	default:
		/* Should not happen. */
		__ASSERT_NO_MSG(false);
		break;
	}

	return res;
}

static void hid_volume_control_send(enum hids_helper_volume_change volume_change)
{
	const char *operation_str = volume_change_to_str(volume_change);
	int err = hids_helper_volume_ctrl(volume_change);

	if (!err) {
		if (operation_str) {
			LOG_INF("%s audio volume", operation_str);
		}
	} else {
		/* HID host not connected or not subscribed. Silently drop HID data. */
	}
}

static void volume_control_btn_handle(uint32_t button_state, uint32_t has_changed)
{
	static enum hids_helper_volume_change volume_change = HIDS_HELPER_VOLUME_CHANGE_NONE;
	enum hids_helper_volume_change new_volume_change = volume_change;

	if (has_changed & VOLUME_UP_BUTTON_MASK) {
		if (button_state & VOLUME_UP_BUTTON_MASK) {
			new_volume_change = HIDS_HELPER_VOLUME_CHANGE_UP;
		} else if (volume_change == HIDS_HELPER_VOLUME_CHANGE_UP) {
			new_volume_change = HIDS_HELPER_VOLUME_CHANGE_NONE;
		}
	}

	if (has_changed & VOLUME_DOWN_BUTTON_MASK) {
		if (button_state & VOLUME_DOWN_BUTTON_MASK) {
			new_volume_change = HIDS_HELPER_VOLUME_CHANGE_DOWN;
		} else if (volume_change == HIDS_HELPER_VOLUME_CHANGE_DOWN) {
			new_volume_change = HIDS_HELPER_VOLUME_CHANGE_NONE;
		}
	}

	if (volume_change != new_volume_change) {
		volume_change = new_volume_change;
		hid_volume_control_send(volume_change);
	}
}

static void fp_adv_mode_btn_handle(uint32_t button_state, uint32_t has_changed)
{
	struct k_work_sync sync;
	uint32_t button_pressed = button_state & has_changed;

	if (button_pressed & FP_ADV_MODE_BUTTON_MASK) {
		k_work_cancel_delayable_sync(&fp_adv_mode_status_led_handle, &sync);
		fp_adv_mode = (fp_adv_mode + 1) % BT_FAST_PAIR_ADV_MODE_COUNT;
		k_work_schedule(&fp_adv_mode_status_led_handle, K_NO_WAIT);
		if (!peer) {
			advertising_start();
		}
	}
}

static void button_changed(uint32_t button_state, uint32_t has_changed)
{
	fp_adv_mode_btn_handle(button_state, has_changed);
	volume_control_btn_handle(button_state, has_changed);
}

void main(void)
{
	bool run_led_on = true;
	int err;

	LOG_INF("Starting Bluetooth Fast Pair example");

	err = hids_helper_init();
	if (err) {
		LOG_ERR("HIDS init failed (err %d)", err);
		return;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	err = settings_load();
	if (err) {
		LOG_ERR("Settings load failed (err: %d)", err);
		return;
	}

	LOG_INF("Settings loaded");

	err = dk_leds_init();
	if (err) {
		LOG_ERR("LEDs init failed (err %d)", err);
		return;
	}

	k_work_init(&bt_adv_restart, bt_adv_restart_fn);
	k_work_init_delayable(&fp_adv_mode_status_led_handle, fp_adv_mode_status_led_handle_fn);

	k_work_schedule(&fp_adv_mode_status_led_handle, K_NO_WAIT);
	advertising_start();

	err = dk_buttons_init(button_changed);
	if (err) {
		LOG_ERR("Buttons init failed (err %d)", err);
		return;
	}

	for (;;) {
		dk_set_led(RUN_STATUS_LED, run_led_on);
		run_led_on = !run_led_on;
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL_MS));
	}
}
