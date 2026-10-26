/*
 * ESP32-P4-WIFI6-M + Raspberry Pi Camera Module 3 (IMX708)
 *
 * 現状: 起動確認用の最小スケルトン。チップ情報・PSRAM 容量を表示するだけ。
 * 実装予定のモジュール (HANDOFF.md のロードマップ参照):
 *   wifi.c   … esp_wifi_remote + esp_hosted (ESP32-C6) で STA 接続
 *   camera.c … esp_video + esp_cam_sensor_imx で IMX708 を初期化、HW JPEG エンコード
 *   http.c   … esp_http_server: GET /capture (JPEG), GET /stream (MJPEG), POST /config
 *   mqtt.c   … esp-mqtt: cam/<id>/cmd を購読、cam/<id>/status を publish
 *   upload.c … 撮影した JPEG を PC の FastAPI (POST /upload) へ送信
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define SCAN_MAX_AP 20

// ESP32-C6 (esp_hosted) との疎通確認: STA で起動して周囲の AP を一覧表示する
static void wifi_scan_test(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // esp_hosted は constructor で自動初期化される設計だが、PlatformIO は
    // WHOLE_ARCHIVE を反映せずそのオブジェクトがリンクされないため明示的に呼ぶ。
    // (二重呼び出しは esp_hosted_init 側で無視される)
    ESP_ERROR_CHECK(esp_hosted_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "C6 STA MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));

    uint16_t num = SCAN_MAX_AP;
    wifi_ap_record_t recs[SCAN_MAX_AP];
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&num, recs));
    ESP_LOGI(TAG, "scan done: %u APs", num);
    for (int i = 0; i < num; i++) {
        ESP_LOGI(TAG, "  ch=%2d rssi=%4d %s", recs[i].primary, recs[i].rssi, (char *)recs[i].ssid);
    }
}

void app_main(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "ESP32-P4 camera project boot");
    ESP_LOGI(TAG, "cores=%d revision=v%d.%d",
             info.cores, info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, "flash=%lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "psram total=%u bytes free=%u bytes",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    wifi_scan_test();

    // TODO: wifi_init_sta();
    // TODO: camera_init();
    // TODO: http_server_start();
    // TODO: mqtt_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
