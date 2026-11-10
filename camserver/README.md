# esp32p4-camserver

ESP32-P4 + IMX708 のカメラを、PC 上の FastAPI サーバーから操作する。シャッター(1920×1080 JPEG を PC に保存)と、
露出・ゲイン・ピント・ホワイトバランスの設定ができる。**設定はボードの LittleFS (`/cfg/config.json`) に保存され、再起動後もそのまま動く。**

```
firmware/   ESP-IDF 5.4 プロジェクト (camctl + 静止画 HTTP サーバー)。esp32p4-cam の imx708_wifi_control を LittleFS 化したもの
pc/         FastAPI サーバー (server.py / board.py / static/index.html)
docs/       SPEC-camera-controls.md (camctl の仕様。保存は §3.6)
```

## ボード
```sh
cp firmware/components/imx_wifi/include/wifi_credentials.h.example firmware/components/imx_wifi/include/wifi_credentials.h   # SSID/PSK を記入
source ../esp32-p4-imx-camera/idfenv.sh
idf -C firmware build            # または cd firmware && idf build
idf -C firmware -p /dev/cu.usbmodem5B910635451 flash monitor
```
IMX708 ドライバは `../esp32-p4-imx-camera` の作業コピーを参照する (`firmware/main/idf_component.yml`)。
Wi-Fi は 2.4 GHz のみ (C6)。起動ログに `camctl: config loaded ...` か `no config.json - defaults` が出る。

## PC
```sh
BOARD_HOST=<ボードのIP> pc/run.sh        # http://127.0.0.1:8080  (API は /docs)
BOARD_HOST=<ボードのIP> pc/run.sh shot -q 95   # UI なしで 1 枚撮影
```
撮影は `pc/captures/` に保存。`board` クエリを省くと `BOARD_HOST` を使う。

| API | 動作 |
|---|---|
| `POST /api/shutter?q=` | フルサイズ撮影して保存 |
| `GET /api/status` | ボード到達性と `config` (`loaded`/`default`/`error`) |
| `GET /api/ctrl`, `/api/ctrl/caps` | 現在値、範囲 |
| `POST /api/ctrl?<key>=<値>` | exposure_mode, gain_mode, exposure_us, gain_index, focus_mode, focus_pos, wb_mode, wb_red, wb_blue |
| `POST /api/af`, `/api/wb/onepush`, `/api/config/reset` | AF、ワンプッシュ WB、設定の初期化 |
| `GET /api/preview.mjpg` | ボードの MJPEG プレビューの中継 |
| `GET /api/captures`, `/captures/<file>` | 撮影履歴 |

## 保存 (LittleFS)
`storage` パーティション (1 MB) を `/cfg` にマウントし、変更の 2 秒後に `config.json.tmp` へ書いて `rename` する。
LittleFS は既存名への `rename` が原子的なので、書き込み中の電源断でも前の設定が残る。マウント失敗時は全 Auto で動く。
旧 SPIFFS で使っていた `storage` を書き込むと一度フォーマットされ、設定は初期化される。

## 状態 (2026-10-29)
ビルド成功。実機で書き込み、LittleFS のマウントとカメラ起動を確認 (`no config.json - defaults`)。
PC サーバーはダミーのボードで API を確認済み。**実機の Wi-Fi 接続・撮影・設定の保存と復元は未確認**
(書き込み時に SSID `ktnyso` が見えず接続できなかった。iPhone のテザリングを 2.4 GHz で起動して再確認する)。
