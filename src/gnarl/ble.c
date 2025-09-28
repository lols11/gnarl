#include "gnarl.h"

#include <unistd.h>

#include <esp_nimble_hci.h>
#include <esp_timer.h>
#include <host/ble_gap.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include "bleprph.h"
#include "esp_pm.h"
#include "host/ble_hs.h"
#include "esp_nimble_hci.h"
#include "nimble/ble.h"
#include "adc.h"
#include "commands.h"
#include "console/console.h"
// #include <host/ble_esp_gap.h>
#define MAX_DATA 150

#define CUSTOM_NAME_SIZE 30
#define STORAGE_NAMESPACE "GNARL"

static uint8_t custom_name[CUSTOM_NAME_SIZE];

void ble_store_ram_init(void);

#define B0(x) ((x) & 0xFF)
#define B1(x) (((x) >> 8) & 0xFF)
#define B2(x) (((x) >> 16) & 0xFF)
#define B3(x) (((x) >> 24) & 0xFF)
#define B4(x) (((x) >> 32) & 0xFF)
#define B5(x) (((x) >> 40) & 0xFF)

#define UUID128_CONST(a32, b16, c16, d16, e48)                \
	BLE_UUID128_INIT(                                         \
		B0(e48), B1(e48), B2(e48), B3(e48), B4(e48), B5(e48), \
		B0(d16), B1(d16),                                     \
		B0(c16), B1(c16),                                     \
		B0(b16), B1(b16),                                     \
		B0(a32), B1(a32), B2(a32), B3(a32), )

static ble_uuid128_t service_uuid = UUID128_CONST(0x0235733b, 0x99c5, 0x4197, 0xb856, 0x69219c2a3845);
static ble_uuid128_t data_uuid = UUID128_CONST(0xc842e849, 0x5028, 0x42e2, 0x867c, 0x016adada9155);
static ble_uuid128_t response_count_uuid = UUID128_CONST(0x6e6c7910, 0xb89e, 0x43a5, 0xa0fe, 0x50c5e2b81f4a);
static ble_uuid128_t timer_tick_uuid = UUID128_CONST(0x6e6c7910, 0xb89e, 0x43a5, 0x78af, 0x50c5e2b86f7e);
static ble_uuid128_t custom_name_uuid = UUID128_CONST(0xd93b2af0, 0x1e28, 0x11e4, 0x8c21, 0x0800200c9a66);
static ble_uuid128_t firmware_version_uuid = UUID128_CONST(0x30d99dc9, 0x7c91, 0x4295, 0xa051, 0x0a104d238cf2);
static ble_uuid128_t led_mode_uuid = UUID128_CONST(0xc6d84241, 0xf1a7, 0x4f9c, 0xa25f, 0xfce16732f14e);

static ble_uuid16_t battery_service_uuid = BLE_UUID16_INIT(0x180F);
static ble_uuid16_t battery_level_uuid = BLE_UUID16_INIT(0x2A19);

static ble_gap_event_fn handle_gap_event;
static uint8_t addr_type;
static ble_gatt_access_fn data_access;
static ble_gatt_access_fn custom_name_access;
static ble_gatt_access_fn led_mode_access;
static ble_gatt_access_fn firmware_version_access;
static ble_gatt_access_fn no_access;

static ble_gatt_access_fn battery_level_access;

static bool connected;
static uint16_t connection_handle;

static uint16_t response_count_notify_handle;
static int response_count_notify_state;
static uint8_t response_count;

static uint16_t timer_tick_notify_handle;
static int timer_tick_notify_state;
static uint8_t timer_tick;
static void timer_tick_callback(void *);

static const struct ble_gatt_svc_def service_list[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &service_uuid.u,
		.characteristics = (struct ble_gatt_chr_def[]){
			{
				.uuid = &data_uuid.u,
				.access_cb = data_access,
				.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
			},
			{
				.uuid = &response_count_uuid.u,
				.access_cb = no_access,
				.val_handle = &response_count_notify_handle,
				.flags = BLE_GATT_CHR_F_NOTIFY,
			},
			{
				.uuid = &timer_tick_uuid.u,
				.access_cb = no_access,
				.val_handle = &timer_tick_notify_handle,
				.flags = BLE_GATT_CHR_F_NOTIFY,
			},
			{
				.uuid = &custom_name_uuid.u,
				.access_cb = custom_name_access,
				.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
			},
			{
				.uuid = &firmware_version_uuid.u,
				.access_cb = firmware_version_access,
				.flags = BLE_GATT_CHR_F_READ,
			},
			{
				.uuid = &led_mode_uuid.u,
				.access_cb = led_mode_access,
				.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
			},
			{.uuid = NULL}},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &battery_service_uuid.u,
		.characteristics = (struct ble_gatt_chr_def[]){
			{
				.uuid = &battery_level_uuid.u,
				.access_cb = battery_level_access,
				.flags = BLE_GATT_CHR_F_READ,
			},
			{.uuid = NULL},
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_END,
	},
};

static void server_init(void)
{
	int err;
	char u[60];

	ble_svc_gap_init();
	ble_svc_gatt_init();

	err = ble_gatts_count_cfg(service_list);
	assert(!err);

	err = ble_gatts_add_svcs(service_list);
	assert(!err);

	ble_uuid_to_str(&service_uuid.u, u);
	ESP_LOGD(TAG, "service UUID %s", u);
	ESP_LOGW(TAG, "AAAAAAAAAAAAAAAAAAAservice UUID %s", u);
	esp_timer_handle_t t;
	esp_timer_create_args_t timer_args = {
		.callback = timer_tick_callback,
	};
	ESP_ERROR_CHECK(esp_timer_create(&timer_args, &t));
	ESP_ERROR_CHECK(esp_timer_start_periodic(t, 60 * SECONDS));
}

static void advertise(void)
{
	struct ble_hs_adv_fields fields; //, fields_ext;
	memset(&fields, 0, sizeof(fields));

	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

	fields.tx_pwr_lvl_is_present = 1;
	fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

	const char *name = ble_svc_gap_device_name();

	fields.uuids128 = &service_uuid;
	fields.num_uuids128 = 1;
	fields.uuids128_is_complete = 1;

	int err = ble_gap_adv_set_fields(&fields);
	if (err)
	{
		ESP_LOGE(TAG, "ble_gap_adv_set_fields err %d", err);
	}
	assert(!err);

	memset(&fields, 0, sizeof(fields));
	fields.name = (uint8_t *)name;
	fields.name_len = strlen(name);
	fields.name_is_complete = 1;
	err = ble_gap_adv_rsp_set_fields(&fields);
	if (err)
	{
		ESP_LOGE(TAG, "ble_gap_adv_set_fields fields_ext, name might be too long, err %d", err);
	}

	// Begin advertising.
	struct ble_gap_adv_params adv;
	memset(&adv, 0, sizeof(adv));
	adv.conn_mode = BLE_GAP_CONN_MODE_UND;
	adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
	// adv.itvl_min = 800;
	// adv.itvl_max = 1000;
	err = ble_gap_adv_start(addr_type, 0, BLE_HS_FOREVER, &adv, handle_gap_event, 0);
	assert(!err);
	//ble_hs_send_vs_event_mask(ESP_BLE_VENDOR_SLEEP_WAKEUP_EVT_MASK);
	//assert(err == 0);
	//static struct ble_gap_event_listener vs_event_listener;
	//err = ble_gap_event_listener_register(&vs_event_listener, handle_gap_event, NULL);
	//assert(err == 0);
	ESP_LOGD(TAG, "advertising started");
}

static int handle_gap_event(struct ble_gap_event *e, void *arg)
{
	switch (e->type)
	{
	case BLE_GAP_EVENT_CONNECT:
		if (e->connect.status != 0)
		{
			ESP_LOGE(TAG, "connection failed");
			advertise();
			return 0;
		}
		connected = true;
		int8_t rssi;
		ble_gap_conn_rssi(e->connect.conn_handle, &rssi);
		set_ble_rssi(rssi);

		// TODO: BLE PARAMS
		const struct ble_gap_upd_params slow = {
			.itvl_min = 60,
			.itvl_max = 80,
			.latency = 2,
			.supervision_timeout = 200};
		// ble_gap_update_params(connection_handle, &slow);
		connection_handle = e->connect.conn_handle;
		ESP_LOGI(TAG, "connected");
		ESP_LOGD(TAG, "connection handle 0x%04X", connection_handle);
		ESP_LOGD(TAG, "response count notify handle 0x%04X", response_count_notify_handle);
		ESP_LOGD(TAG, "timer tick notify handle 0x%04X", timer_tick_notify_handle);

		break;
	case BLE_GAP_EVENT_DISCONNECT:
		connected = false;
		set_ble_disconnected();
		ESP_LOGD(TAG, "disconnected (0x%x)", e->disconnect.reason);
		advertise();
		break;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		ESP_LOGD(TAG, "advertising complete (0x%x)", e->adv_complete.reason);
		advertise();
		break;
	case BLE_GAP_EVENT_SUBSCRIBE:
		if (e->subscribe.attr_handle == response_count_notify_handle)
		{
			ESP_LOGD(TAG, "notify %d for response count", e->subscribe.cur_notify);
			response_count_notify_state = e->subscribe.cur_notify;
			break;
		}
		if (e->subscribe.attr_handle == timer_tick_notify_handle)
		{
			ESP_LOGD(TAG, "notify %d for timer tick", e->subscribe.cur_notify);
			timer_tick_notify_state = e->subscribe.cur_notify;
			break;
		}
		ESP_LOGD(TAG, "notify %d for unknown handle %04X", e->subscribe.cur_notify, e->subscribe.attr_handle);
		break;
	case BLE_GAP_EVENT_VS_HCI:
	{
		const struct ble_hci_ev_vs *ev = e->vs_hci.ev;
		if (ev->id == BLE_HCI_VS_SUBEV_LE_SLEEP_WAKE_UP)
		{
			ESP_LOGI(TAG, "Controller woke up from modem-sleep");
		}
		break;
	}

	default:
		ESP_LOGD(TAG, "GAP event %d", e->type);
		break;
	}
	return 0;
}

static void sync_callback(void)
{
	int err;

	err = ble_hs_util_ensure_addr(0);
	assert(!err);

	err = ble_hs_id_infer_auto(0, &addr_type);
	assert(!err);

	uint8_t addr[6];
	ble_hs_id_copy_addr(addr_type, addr, 0);

	ESP_LOGD(TAG, "device address: %02x:%02x:%02x:%02x:%02x:%02x",
			 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

	advertise();
}

static uint8_t data_in[MAX_DATA];
static uint16_t data_in_len;

static uint8_t data_out[MAX_DATA];
static uint16_t data_out_len;

static void response_notify(void)
{
	response_count++;
	if (!response_count_notify_state)
	{
		ESP_LOGD(TAG, "not notifying for response count %d", response_count);
		return;
	}
	struct os_mbuf *om = ble_hs_mbuf_from_flat(&response_count, sizeof(response_count));
	int err = ble_gattc_notify_custom(connection_handle, response_count_notify_handle, om);
	assert(!err);
	ESP_LOGD(TAG, "notify for response count %d", response_count);
}

void send_code(const uint8_t code)
{
	ESP_LOGD(TAG, "send_code %02X", code);
	data_out[0] = code;
	data_out_len = 1;
	response_notify();
}

void send_bytes(const uint8_t *buf, int count)
{
	data_out[0] = RESPONSE_CODE_SUCCESS;
	memcpy(data_out + 1, buf, count);
	data_out_len = count + 1;
	response_notify();
}

static void timer_tick_callback(void *arg)
{
	timer_tick++;
	ESP_LOGD(TAG, "timer tick %d", timer_tick);
	if (!timer_tick_notify_state)
	{
		if (connected)
		{
			ESP_LOGD(TAG, "not notifying for timer tick");
		}
		return;
	}
	struct os_mbuf *om = ble_hs_mbuf_from_flat(&timer_tick, sizeof(timer_tick));
	int err = ble_gattc_notify_custom(connection_handle, timer_tick_notify_handle, om);
	assert(!err);
	ESP_LOGD(TAG, "notify for timer tick");
}

static int data_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	int err;
	int8_t rssi;
	assert(ble_uuid_cmp(ctxt->chr->uuid, &data_uuid.u) == 0);
	switch (ctxt->op)
	{
	case BLE_GATT_ACCESS_OP_READ_CHR:
		ESP_LOGD(TAG, "data_access: sending %d bytes from pump to phone", data_out_len);
		if (os_mbuf_append(ctxt->om, data_out, data_out_len) != 0)
		{
			return BLE_ATT_ERR_INSUFFICIENT_RES;
		}
		return 0;
	case BLE_GATT_ACCESS_OP_WRITE_CHR:
		err = ble_hs_mbuf_to_flat(ctxt->om, data_in, sizeof(data_in), &data_in_len);
		assert(!err);
		ESP_LOGD(TAG, "data_access: command received");
		ble_gap_conn_rssi(conn_handle, &rssi);
		rfspy_command(data_in, data_in_len, (int)rssi);
		return 0;
	default:
		assert(0);
	}
	return 0;
}

static void read_custom_name(void)
{
	ESP_LOGD(TAG, "read_custom_name from nvs");
	nvs_handle my_handle;
	esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "read_custom_name: nvs_open: %s", esp_err_to_name(err));
		return;
	}
	size_t required_size = CUSTOM_NAME_SIZE;
	err = nvs_get_blob(my_handle, "custom_name", custom_name, &required_size);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "read_custom_name: nvs_get_blob: %s", esp_err_to_name(err));
	}
	else
	{
		ESP_LOGD(TAG, "read_custom_name success: %s", custom_name);
	}
	nvs_close(my_handle);
}

static void write_custom_name(void)
{
	ESP_LOGD(TAG, "write_custom_name to nvs");
	nvs_handle my_handle;
	esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "write_custom_name: nvs_open: %s", esp_err_to_name(err));
		return;
	}
	err = nvs_set_blob(my_handle, "custom_name", custom_name, CUSTOM_NAME_SIZE);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "write_custom_name: nvs_set_blob: %s", esp_err_to_name(err));
		nvs_close(my_handle);
		return;
	}
	err = nvs_commit(my_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "write_custom_name: nvs_commit: %s", esp_err_to_name(err));
	}
	nvs_close(my_handle);
}

static int custom_name_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	int err;
	uint16_t custom_name_len = strlen((char *)custom_name);
	assert(custom_name_len <= sizeof(custom_name));
	assert(ble_uuid_cmp(ctxt->chr->uuid, &custom_name_uuid.u) == 0);
	switch (ctxt->op)
	{
	case BLE_GATT_ACCESS_OP_READ_CHR:
		ESP_LOGD(TAG, "custom_name_access: sending %s to phone", custom_name);
		if (os_mbuf_append(ctxt->om, custom_name, custom_name_len) != 0)
		{
			return BLE_ATT_ERR_INSUFFICIENT_RES;
		}
		return 0;
	case BLE_GATT_ACCESS_OP_WRITE_CHR:
		err = ble_hs_mbuf_to_flat(ctxt->om, custom_name, sizeof(custom_name), &custom_name_len);
		custom_name[custom_name_len] = 0;
		ESP_LOGD(TAG, "custom_name_access: received %s from phone", custom_name);
		assert(!err);
		write_custom_name();
		esp_restart();
		return 0;
	default:
		assert(0);
	}
	return 0;
}

static int firmware_version_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	assert(ble_uuid_cmp(ctxt->chr->uuid, &firmware_version_uuid.u) == 0);
	assert(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR);
	ESP_LOGD(TAG, "BLE firmware version = %s", BLE_RFSPY_VERSION);
	if (os_mbuf_append(ctxt->om, (const uint8_t *)BLE_RFSPY_VERSION, strlen(BLE_RFSPY_VERSION)) != 0)
	{
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	return 0;
}

static int battery_level_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	assert(ble_uuid_cmp(ctxt->chr->uuid, &battery_level_uuid.u) == 0);
	assert(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR);
	uint8_t battery_level = battery_percent(get_battery_voltage());
	ESP_LOGD(TAG, "battery_level_access: %d%%", battery_level);
	if (os_mbuf_append(ctxt->om, &battery_level, sizeof(battery_level)) != 0)
	{
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	return 0;
}

static uint8_t led_mode;

static int led_mode_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	int err;
	uint16_t n;
	assert(ble_uuid_cmp(ctxt->chr->uuid, &led_mode_uuid.u) == 0);
	switch (ctxt->op)
	{
	case BLE_GATT_ACCESS_OP_READ_CHR:
		ESP_LOGD(TAG, "led_mode_access: mode = %d", led_mode);
		if (os_mbuf_append(ctxt->om, &led_mode, sizeof(led_mode)))
		{
			return BLE_ATT_ERR_INSUFFICIENT_RES;
		}
		return 0;
	case BLE_GATT_ACCESS_OP_WRITE_CHR:
		err = ble_hs_mbuf_to_flat(ctxt->om, &led_mode, sizeof(led_mode), &n);
		assert(!err);
		ESP_LOGD(TAG, "led_mode_access: set mode = %d", led_mode);
		return 0;
	default:
		assert(0);
	}
	return 0;
}

static int no_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	char u[60];
	ble_uuid_to_str(ctxt->chr->uuid, u);
	ESP_LOGE(TAG, "should not happen: op %d, attr handle %04X, uuid %s", ctxt->op, attr_handle, u);
	return 0;
}

static void host_task(void *arg)
{
	nimble_port_run();
}

void gnarl_init(void)
{
	start_gnarl_task();

	ESP_ERROR_CHECK(nvs_flash_init());
	nimble_port_init();

	ble_hs_cfg.sync_cb = sync_callback;

	server_init();

	read_custom_name();

	if (custom_name[0])
	{
		int err = ble_svc_gap_device_name_set((char *)custom_name);
		assert(!err);
	}

	ble_store_ram_init();
	nimble_port_freertos_init(host_task);
}
