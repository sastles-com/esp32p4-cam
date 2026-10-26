# esp32p4-cam

Waveshare **ESP32-P4-WIFI6-M** と **Raspberry Pi Camera Module 3 (IMX708)** で撮影し、
Wi-Fi 経由で PC に画像を送る。撮影や設定は **MQTT** または **HTTP(FastAPI / Web UI)** で操作する。

> ステータス: **雛形段階**。ハードウェア未着・未ビルド。
> 作業を引き継ぐ場合は先に [HANDOFF.md](HANDOFF.md) を読んでください。

## ハードウェア

| 部品 | 型番 | 備考 |
|---|---|---|
| MCU ボード | Waveshare ESP32-P4-WIFI6-M (SKU 31647) | ESP32-P4NRW32 (32MB PSRAM) + 32MB NOR Flash + ESP32-C6-MINI-1 (SDIO 接続の Wi-Fi 6 / BLE 5)。-M はピンヘッダーはんだ付け済み |
| カメラ | Raspberry Pi Camera Module 3 標準画角 (SC0872 / スイッチサイエンス RPI-SC1223) | IMX708 12MP、DW9807 VCM オートフォーカス。15-22pin と 15-15pin の FPC ケーブルが付属 |

- カメラ接続: ボードの MIPI-CSI (2-lane) コネクタ。どちらのケーブルが合うかは実物で確認する(HANDOFF.md 参照)
- 書き込み・電源: USB Type-C

## 構成

```
┌────────── ESP32-P4-WIFI6-M ──────────┐        ┌──────────── PC ────────────┐
│ IMX708 ─MIPI-CSI→ ISP → HW JPEG/H.264 │        │ Mosquitto (MQTT broker)     │
│ esp_http_server: /capture /stream     │ ─HTTP→ │ FastAPI: /upload, Web UI    │
│ esp-mqtt: cam/<id>/cmd を購読         │ ←MQTT─ │   └ MQTT にコマンドを publish │
│ Wi-Fi: ESP32-C6 (esp_hosted, SDIO)    │        │                             │
└───────────────────────────────────────┘        └─────────────────────────────┘
```

## ディレクトリ

```
esp32p4-cam/
├── platformio.ini          # pioarduino プラットフォーム + ESP-IDF
├── CMakeLists.txt          # ESP-IDF プロジェクト定義
├── sdkconfig.defaults      # PSRAM / Flash / カメラ / esp_hosted (TODO あり)
├── partitions.csv          # 32MB Flash 用 (app 8MB + storage 16MB)
├── src/
│   ├── CMakeLists.txt
│   ├── idf_component.yml   # esp_video, esp_cam_sensor_imx, esp_hosted, esp_wifi_remote
│   ├── main.c              # 起動確認スケルトン
│   └── secrets.example.h   # → secrets.h にコピーして Wi-Fi/MQTT 設定
├── pc/                     # PC 側 (FastAPI)
│   ├── server.py
│   └── requirements.txt
├── README.md
└── HANDOFF.md              # 引き継ぎ資料 (経緯・決定事項・TODO・落とし穴)
```

## ビルド

前提: VS Code + PlatformIO IDE 拡張、または `pip install platformio`

```bash
cp src/secrets.example.h src/secrets.h   # 値を編集
pio run                                  # 初回はプラットフォーム・IDF・コンポーネントの取得に時間がかかる
pio run -t upload
pio device monitor
pio run -t menuconfig                    # カメラ/esp_hosted の Kconfig 設定
```

## PC 側

```bash
cd pc
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn server:app --host 0.0.0.0 --port 8000
```

MQTT ブローカーは Mosquitto などを別途起動する。

## MQTT トピック案

| トピック | 方向 | ペイロード例 |
|---|---|---|
| `cam/<id>/cmd` | PC → ESP | `{"action":"capture"}` / `{"action":"set","resolution":"1920x1080","af":"auto"}` |
| `cam/<id>/status` | ESP → PC | `{"state":"online","ip":"192.168.1.50","rssi":-55}` (LWT で offline) |
| `cam/<id>/event` | ESP → PC | `{"captured":"/upload/xxx.jpg","bytes":260000}` |

## HTTP エンドポイント案 (ESP 側)

| メソッド | パス | 内容 |
|---|---|---|
| GET | `/capture` | JPEG 1 枚 |
| GET | `/stream` | MJPEG (`multipart/x-mixed-replace`) |
| GET/POST | `/config` | 解像度・露出・AF の取得/設定 (JSON) |
| GET | `/status` | 稼働状態 |

## 参考資料

- Waveshare ESP32-P4-WIFI6 docs: https://docs.waveshare.com/ESP32-P4-WIFI6
- esp_cam_sensor_imx (IMX708 ドライバ): https://components.espressif.com/components/mushbraindave/esp_cam_sensor_imx
- 同リポジトリ: https://github.com/mushBrainDave/esp32-p4-imx-camera
- FabScene 記事: https://fabscene.com/new/make/nfc-cassette-tape-display-home-assistant/
- nWorld「ESP32-P4でRaspberry Pi Cameraを使ってみた。」: https://firtel.blogspot.com/2026/09/esp32-p4-raspberry-pi-camera.html
- pioarduino: https://github.com/pioarduino/platform-espressif32
