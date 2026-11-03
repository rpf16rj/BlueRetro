/*
 * Copyright (c) 2026, rpf16rj
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedded web configuration UI: WiFi SoftAP + HTTP/WebSocket API
 * exposing the same operations as the BLE config service.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_app_desc.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <mbedtls/base64.h>
#include <cJSON.h>
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "adapter/gameid.h"
#include "adapter/memory_card.h"
#include "bluetooth/host.h"
#include "bluetooth/hci.h"
#include "system/delay.h"
#include "system/fs.h"
#include "system/manager.h"
#include "webui.h"

#define WIFI_AP_SSID     "BlueRetro"
#define WIFI_AP_PASS     "blueretro"
#define WIFI_AP_CHAN     6
#define WIFI_AP_MAX_CONN 4
#define MC_IMG_SIZE      0x20000 /* 128 KiB */
#define WS_MAX_REQ       6144
#define RAW_CHUNK        768

#define BR_ABI_VER 2

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t server = NULL;

static void j_add_b64(cJSON *o, const char *key, const uint8_t *src, uint32_t len) {
    uint32_t cap = 4 * ((len + 2) / 3) + 1;
    char *b = malloc(cap);
    if (b == NULL) {
        return;
    }
    size_t olen = 0;
    mbedtls_base64_encode((uint8_t *)b, cap, &olen, src, len);
    b[olen] = 0;
    cJSON_AddStringToObject(o, key, b);
    free(b);
}

static uint32_t j_get_b64(cJSON *item, uint8_t *dst, uint32_t dst_len) {
    size_t olen = 0;
    if (!cJSON_IsString(item)) {
        return 0;
    }
    mbedtls_base64_decode(dst, dst_len, &olen,
        (const uint8_t *)item->valuestring, strlen(item->valuestring));
    return (uint32_t)olen;
}

static void ws_reply(httpd_req_t *req, cJSON *req_json, cJSON *rsp_json) {
    cJSON *id = cJSON_GetObjectItem(req_json, "id");
    cJSON_AddItemToObject(rsp_json, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    char *txt = cJSON_PrintUnformatted(rsp_json);
    if (txt) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)txt,
            .len = strlen(txt),
            .final = true,
        };
        httpd_ws_send_frame(req, &frame);
        free(txt);
    }
}

static void ws_reply_err(httpd_req_t *req, cJSON *req_json, const char *err) {
    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "ok", false);
    cJSON_AddStringToObject(rsp, "err", err);
    ws_reply(req, req_json, rsp);
    cJSON_Delete(rsp);
}

static void dev_list_reply(httpd_req_t *req, cJSON *req_json) {
    cJSON *rsp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(rsp, "devs");

    for (int32_t i = 0; i < BT_MAX_DEV; i++) {
        struct bt_dev *device = NULL;
        if (bt_host_get_dev_from_id(i, &device) || device == NULL) {
            continue;
        }
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "id", i);
        cJSON_AddNumberToObject(d, "out", device->ids.out_idx);
        cJSON_AddNumberToObject(d, "type", device->ids.type);
        cJSON_AddNumberToObject(d, "subtype", device->ids.subtype);
        cJSON_AddBoolToObject(d, "active",
            atomic_test_bit(&device->flags, BT_DEV_HID_INIT_DONE));
        cJSON_AddBoolToObject(d, "ble",
            atomic_test_bit(&device->flags, BT_DEV_IS_BLE));
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
            device->remote_bdaddr[5], device->remote_bdaddr[4],
            device->remote_bdaddr[3], device->remote_bdaddr[2],
            device->remote_bdaddr[1], device->remote_bdaddr[0]);
        cJSON_AddStringToObject(d, "bdaddr", addr);
        if (device->name && device->name->name[0]) {
            cJSON_AddStringToObject(d, "name", device->name->name);
        }
        cJSON_AddItemToArray(arr, d);
    }
    cJSON_AddBoolToObject(rsp, "ok", true);
    cJSON_AddBoolToObject(rsp, "inq", bt_hci_get_inquiry());
    ws_reply(req, req_json, rsp);
    cJSON_Delete(rsp);
}

static void sys_delayed_reset(void *arg) {
    (void)arg;
    delay_us(500000);
    sys_mgr_cmd(SYS_MGR_CMD_ADAPTER_RST);
    vTaskDelete(NULL);
}

static void handle_ws_frame(httpd_req_t *req, httpd_ws_frame_t *frame) {
    cJSON *j = cJSON_ParseWithLength((const char *)frame->payload, frame->len);
    if (j == NULL) {
        cJSON *rsp = cJSON_CreateObject();
        cJSON_AddBoolToObject(rsp, "ok", false);
        cJSON_AddStringToObject(rsp, "err", "bad json");
        httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT};
        char *txt = cJSON_PrintUnformatted(rsp);
        if (txt) {
            f.payload = (uint8_t *)txt;
            f.len = strlen(txt);
            f.final = true;
            httpd_ws_send_frame(req, &f);
            free(txt);
        }
        cJSON_Delete(rsp);
        return;
    }

    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(j, "op"));
    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "ok", true);

    if (op == NULL) {
        cJSON_SetBoolValue(cJSON_GetObjectItem(rsp, "ok"), false);
        cJSON_AddStringToObject(rsp, "err", "no op");
    }
    else if (!strcmp(op, "info")) {
        const esp_app_desc_t *app_desc = esp_app_get_description();
        bt_addr_le_t bdaddr = {0};
        bt_hci_get_le_local_addr(&bdaddr);
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
            bdaddr.a.val[5], bdaddr.a.val[4], bdaddr.a.val[3],
            bdaddr.a.val[2], bdaddr.a.val[1], bdaddr.a.val[0]);
        cJSON_AddNumberToObject(rsp, "abi", BR_ABI_VER);
        cJSON_AddStringToObject(rsp, "fw", app_desc->version);
        cJSON_AddStringToObject(rsp, "name", app_desc->project_name);
        cJSON_AddStringToObject(rsp, "bdaddr", addr);
        cJSON_AddStringToObject(rsp, "gameid", gid_get());
        cJSON_AddNumberToObject(rsp, "cfgsrc", config_get_src());
        cJSON_AddNumberToObject(rsp, "sysid", wired_adapter.system_id);
        cJSON_AddNumberToObject(rsp, "mcsize", MC_IMG_SIZE);
        cJSON_AddBoolToObject(rsp, "inq", bt_hci_get_inquiry());
    }
    else if (!strcmp(op, "glbl_get")) {
        j_add_b64(rsp, "data", (uint8_t *)&config.global_cfg, sizeof(config.global_cfg));
    }
    else if (!strcmp(op, "glbl_set")) {
        uint8_t buf[sizeof(config.global_cfg)] = {0};
        if (j_get_b64(cJSON_GetObjectItem(j, "data"), buf, sizeof(buf)) != sizeof(buf)) {
            ws_reply_err(req, j, "bad data");
            goto out;
        }
        memcpy(&config.global_cfg, buf, sizeof(config.global_cfg));
        config_update(config_get_src());
    }
    else if (!strcmp(op, "out_get")) {
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "port"));
        if (id < 0 || id >= WIRED_MAX_DEV) {
            ws_reply_err(req, j, "bad port");
            goto out;
        }
        j_add_b64(rsp, "data", (uint8_t *)&config.out_cfg[id], sizeof(config.out_cfg[0]));
    }
    else if (!strcmp(op, "out_set")) {
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "port"));
        uint8_t buf[sizeof(config.out_cfg[0])] = {0};
        if (id < 0 || id >= WIRED_MAX_DEV
            || j_get_b64(cJSON_GetObjectItem(j, "data"), buf, sizeof(buf)) != sizeof(buf)) {
            ws_reply_err(req, j, "bad data");
            goto out;
        }
        memcpy(&config.out_cfg[id], buf, sizeof(config.out_cfg[0]));
        config_update(config_get_src());
    }
    else if (!strcmp(op, "in_get")) {
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "dev"));
        if (id < 0 || id >= WIRED_MAX_DEV) {
            ws_reply_err(req, j, "bad dev");
            goto out;
        }
        j_add_b64(rsp, "data", (uint8_t *)&config.in_cfg[id], sizeof(config.in_cfg[0]));
    }
    else if (!strcmp(op, "in_set")) {
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "dev"));
        uint8_t *buf = malloc(sizeof(config.in_cfg[0]));
        if (buf == NULL || id < 0 || id >= WIRED_MAX_DEV
            || j_get_b64(cJSON_GetObjectItem(j, "data"), buf, sizeof(config.in_cfg[0])) == 0) {
            free(buf);
            ws_reply_err(req, j, "bad data");
            goto out;
        }
        memcpy(&config.in_cfg[id], buf, sizeof(config.in_cfg[0]));
        free(buf);
        config_update(config_get_src());
    }
    else if (!strcmp(op, "dev_list")) {
        cJSON_Delete(rsp);
        dev_list_reply(req, j);
        goto out;
    }
    else if (!strcmp(op, "dev_disc")) {
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "dev"));
        struct bt_dev *device = NULL;
        if (bt_host_get_dev_from_id(id, &device) || device == NULL) {
            ws_reply_err(req, j, "bad dev");
            goto out;
        }
        bt_hci_disconnect(device);
    }
    else if (!strcmp(op, "pair")) {
        bt_hci_start_inquiry();
    }
    else if (!strcmp(op, "mc_read")) {
        uint32_t off = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "off"));
        uint32_t len = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "len"));
        if (off >= MC_IMG_SIZE || len > RAW_CHUNK || off + len > MC_IMG_SIZE || len == 0) {
            ws_reply_err(req, j, "bad range");
            goto out;
        }
        uint8_t *buf = malloc(len);
        if (buf == NULL) {
            ws_reply_err(req, j, "oom");
            goto out;
        }
        mc_read(off, buf, len);
        j_add_b64(rsp, "data", buf, len);
        free(buf);
    }
    else if (!strcmp(op, "mc_write")) {
        uint32_t off = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "off"));
        uint8_t *buf = malloc(RAW_CHUNK);
        uint32_t len = j_get_b64(cJSON_GetObjectItem(j, "data"), buf, RAW_CHUNK);
        if (buf == NULL || off >= MC_IMG_SIZE || len == 0 || off + len > MC_IMG_SIZE) {
            free(buf);
            ws_reply_err(req, j, "bad data");
            goto out;
        }
        mc_write(off, buf, len);
        free(buf);
        cJSON_AddNumberToObject(rsp, "wrote", len);
    }
    else if (!strcmp(op, "dir")) {
        cJSON *files = cJSON_AddArrayToObject(rsp, "files");
        DIR *d = opendir(ROOT);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                cJSON_AddItemToArray(files, cJSON_CreateString(e->d_name));
            }
            closedir(d);
        }
    }
    else if (!strcmp(op, "del")) {
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(j, "file"));
        char tmp_str[64];
        if (file == NULL || strchr(file, '/') || strlen(file) > 48) {
            ws_reply_err(req, j, "bad file");
            goto out;
        }
        snprintf(tmp_str, sizeof(tmp_str), "%s/%s", ROOT, file);
        remove(tmp_str);
    }
    else if (!strcmp(op, "sys")) {
        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(j, "cmd"));
        if (!strcmp(cmd ? cmd : "", "reset")) {
            ws_reply(req, j, rsp);
            xTaskCreate(sys_delayed_reset, "sys_rst", 1024, NULL, 5, NULL);
            goto out;
        }
        else if (!strcmp(cmd ? cmd : "", "sleep")) {
            sys_mgr_cmd(SYS_MGR_CMD_DEEP_SLEEP);
        }
        else if (!strcmp(cmd ? cmd : "", "factory")) {
            sys_mgr_cmd(SYS_MGR_CMD_FACTORY_RST);
        }
        else {
            ws_reply_err(req, j, "bad cmd");
            goto out;
        }
    }
    else {
        cJSON_SetBoolValue(cJSON_GetObjectItem(rsp, "ok"), false);
        cJSON_AddStringToObject(rsp, "err", "unknown op");
    }

    ws_reply(req, j, rsp);
out:
    cJSON_Delete(rsp);
    cJSON_Delete(j);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        printf("# %s: WS handshake done\n", __FUNCTION__);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    uint8_t *buf = calloc(1, WS_MAX_REQ);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK || frame.len > WS_MAX_REQ - 1) {
        free(buf);
        return ret ? ret : ESP_FAIL;
    }
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    handle_ws_frame(req, &frame);
    free(buf);
    return ESP_OK;
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
        index_html_end - index_html_start);
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota_hdl = 0;
    esp_err_t err;
    char buf[2048];
    int remaining = req->content_len;

    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_hdl);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(ota_hdl);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota_hdl, buf, n) != ESP_OK) {
            esp_ota_abort(ota_hdl);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_FAIL;
        }
        remaining -= n;
    }
    err = esp_ota_end(ota_hdl);
    if (err != ESP_OK || esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(sys_delayed_reset, "sys_rst", 1024, NULL, 5, NULL);
    return ESP_OK;
}

int32_t webui_init(void) {
    uint8_t mac[6] = {0};
    char ssid[32];
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        printf("# %s: event loop fail %d\n", __FUNCTION__, ret);
        return -1;
    }
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        printf("# %s: wifi init fail %d\n", __FUNCTION__, ret);
        return -1;
    }
    esp_efuse_mac_get_default(mac);
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", WIFI_AP_SSID, mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = WIFI_AP_CHAN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = WIFI_AP_MAX_CONN,
            .beacon_interval = 100,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);
    strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASS, sizeof(ap_cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        printf("# %s: wifi start fail %d\n", __FUNCTION__, ret);
        return -1;
    }
    printf("# %s: AP started SSID: %s\n", __FUNCTION__, ssid);

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.stack_size = 8192;
    httpd_cfg.max_uri_handlers = 8;
    httpd_cfg.lru_purge_enable = true;

    ret = httpd_start(&server, &httpd_cfg);
    if (ret != ESP_OK) {
        printf("# %s: httpd start fail %d\n", __FUNCTION__, ret);
        return -1;
    }

    httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    httpd_uri_t uri_ota = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_ws);
    httpd_register_uri_handler(server, &uri_ota);

    printf("# %s: webui ready on http://192.168.4.1\n", __FUNCTION__);
    return 0;
}
