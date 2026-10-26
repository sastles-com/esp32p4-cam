# HANDOFF — esp32p4-cam

別の LLM / 開発者がこのプロジェクトを引き継ぐための資料。
**最終更新: 2026-10-15** ・ ステータス: ① MF 静止画ツールが実機で動作 (ブラウザでの操作感は未確認)。② AF 動画、③ MF 動画は未着手。

## 00. 2026-10-15: アプリ ① MF 静止画ツール

作るアプリは 3 つ: ① MF 静止画、② AF 動画、③ MF 動画。ファームはアプリごとに別にし、切り替えは毎回 USB で書き込む。録画は PC に保存する (SD は Wi-Fi と同時に使えない)。

- **ボード側**: `esp32-p4-imx-camera/components/esp_cam_sensor_imx/examples/imx708_wifi_control`
  - `imx708_wifi_snapshot` の複製に `main/camctl.c` を足したもの
  - ビルドは `source esp32-p4-imx-camera/idfenv.sh` → `idf build` → `idf -p /dev/cu.usbmodem5B910635451 flash`。`idf.py` を使い、PlatformIO は使っていない
  - `CONFIG_ESP_VIDEO_ISP_PIPELINE_CONTROL_CAMERA_MOTOR=n`: レンズはアプリが動かす。AF は 1 回だけ合わせる方式
  - API: `GET/POST /ctrl`、`GET /ctrl/caps`、`POST /af`、`POST /config/reset`
  - 設定は SPIFFS (`storage` 領域、1MB) 上の `/cfg/config.json` に保存し、再起動しても保持する
- **PC 側**: `pc/.venv/bin/python pc/camtool.py serve --board 172.20.10.6` → http://127.0.0.1:8080
  - 露出 Auto/Manual、シャッター、明るさ (「自動」= シャッター優先)、ピント Auto/Manual、AF、品質 100 の撮影
- **仕様と結果**: `docs/SPEC-camera-controls.md` (§6 に実測値と、仕様から変えた点)
- **わかったこと**:
  - シャッター優先は「AE を止めて、アプリがゲインを合わせる」方式 (B) で実現した。AGC の露出範囲を 1 点に固定する方式 (A) では、ゲインが調整されなくなる
  - AE には明るさの保持範囲があり、元の JSON (50〜150) は広すぎた。そのため専用のコピー `ipa/imx708_control.json` で 52〜68 に狭めた
  - 緑かぶりの原因は、IPA の AWB がほとんど補正していなかったこと (`min_*_gain_step` が 0.5)。専用 JSON から AWB を外し、`camctl` が WB を持つ形にした。Auto (グレーワールド)、ワンプッシュ、Manual (赤・青のゲイン) の 3 モード。詳しくは SPEC §6.4
  - P4 の画像幅の上限で、静止画は最大 1920×1080
- **次**: ブラウザでの操作感を確認する → ② AF 動画 / ③ MF 動画 (`imx708_wifi_video` をもとに、`camctl` を再利用)

## 0. 実機確認の結果 (2026-10-08)

- チップ ESP32-P4 **rev v1.3** → `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` を有効化済み。Flash 32MB (GigaDevice)、PSRAM 32MB を確認。
- USB シリアルは WCH CH343 (`/dev/cu.usbmodem*`)。
- ESP32-C6 を SDIO で認識(CLK18 CMD19 D0-3=14-17 Reset=54、esp_hosted の既定値のまま)。Wi-Fi スキャンで AP を取得できた。
- C6 側ファームは出荷時 `0.0.0` だったが、**esp_hosted 2.12.12 に OTA 更新済み**(SDIO 経由、`/Users/katano/work/ESP32-P4/c6-ota`)。ホスト側も 2.12.12 に合わせること。
- カメラ (IMX708 + DW9807) は `esp32-p4-imx-camera` リポジトリのサンプルで、静止画撮影・AF・Wi-Fi 接続 (802.11ax) まで動作確認済み(ESP-IDF 5.4.0 + `idf.py`)。
- 最初に AliExpress で購入したボードは C6 非搭載だった。現在は別ボードを使用中。

### PC ツール `pc/camtool.py`(プレビュー + シャッター)

- `pc/.venv/bin/python pc/camtool.py serve --board <ボードIP>` → http://127.0.0.1:8080 。低解像度の MJPEG プレビューを見ながら、シャッター(Space)でフルサイズ (1920×1080) を `pc/captures/` に保存する。CLI だけで撮るなら `camtool.py shot --board <IP> -q 95`。
- ボード側は `esp32-p4-imx-camera/.../examples/imx708_wifi_snapshot` を書き込む(`:81/stream.mjpg?q=&n=` と `:80/snapshot.jpg?q=`)。JPEG エンジンは P4 に 1 つしかないので、起動時に作った 1 つを排他制御して共有している。
- 動作確認: ダミーのボードでツール側(API・CLI・保存・エラー処理)は確認済み。**実機でのプレビュー + シャッターの併用、ブラウザ画面の操作は未確認**(ボードが Wi-Fi に繋がらない状態だった)。
- H.264 (`imx708_wifi_video`) はテザリング回線では 3 Mbit/s が追いつかず数秒遅れる。遅れを抑えたい用途は MJPEG を使う。

### ビルドでハマった点

- PATH 上の `pio` (pyenv, 6.1.19) は古く pioarduino 非対応。`~/.platformio/penv/bin/pio` (6.2.0) を使う。
- `~/.platformio/packages/tool-cmake` が Homebrew cmake へのシンボリックリンクで、インストールが失敗した。`tool-cmake.homebrew-link` に名前を変えて退避済み。
- esp_hosted 3.x は `"SHELL:-include ..."` を使っており、PlatformIO でコンパイルコマンドが壊れる。そのため `^2` に固定した(2.12.13)。
- esp_hosted の constructor による自動初期化は、PlatformIO が WHOLE_ARCHIVE を反映しないため動かない。`esp_hosted_init()` を明示的に呼ぶ。

---

## 1. ゴール

1. Raspberry Pi Camera Module 3 (IMX708) で撮影する
2. 撮影結果を Wi-Fi 経由で PC に送る(静止画 JPEG。将来はライブ映像も)
3. PC から **MQTT** と **HTTP(FastAPI / Web UI)** で撮影・設定を操作する
4. 開発環境は **PlatformIO**

## 2. 決定事項と理由

| 決定 | 理由 |
|---|---|
| プラットフォームは **pioarduino** (`platform-espressif32` フォーク) | 公式 `platformio/espressif32` は ESP32-P4 非対応 |
| フレームワークは **ESP-IDF** (`framework = espidf`) | IMX708 ドライバが IDF コンポーネント。Waveshare も P4 では IDF を推奨 |
| board = `esp32-p4` を流用し Flash を 32MB に上書き | 汎用定義は 4MB。WIFI6-M 専用の定義は無い |
| カメラドライバは `mushbraindave/esp_cam_sensor_imx` | Espressif 標準ドライバは IMX708 非対応。作者は **同じ Waveshare ESP32-P4-WIFI6 + ESP-IDF v5.4.0** で動作確認済み |
| FastAPI は **PC 側**で動かす | ESP 側は `esp_http_server` と `esp-mqtt`(どちらも IDF 標準) |
| 静止画は HW JPEG、ライブ映像は H.264 を検討 | 実測の Wi-Fi 速度は 6〜7 Mbps 程度なので、1080p MJPEG を高 fps で流すのは厳しい |

### 検討して保留にした案
- **Arduino フレームワーク**: Arduino core 3.3.11 (IDF 5.5.4) で `ESP_Video.h` が使えるようになり、OV5647 なら簡単に動く(nWorld の記事)。ただし IMX708 は ESP-Video 標準では非対応。`framework = arduino, espidf` の併用は可能だが、ドライバの検証環境 (IDF 5.4) との差でハマる可能性がある。IDF で動いた後の選択肢として残す。

## 3. 既知の制約・事実

- **IMX708 on P4 の上限**: 2×2 ビニングの 1920×1080 RAW10 で 28fps(P4 側の幅制限のため 2304 幅からデジタルクロップ)。12MP フル解像度・HDR は不可。ドライバ v0.3 以降は複数解像度の実行時切替、2〜56fps、長時間露光、反転に対応。
- **AF**: DW9807 VCM (I2C 0x0C) を `esp_ipa` の AF アルゴリズムで駆動。位相差 AF は非対応(コントラスト AF のため遅め)。
- **Wi-Fi 実測** (ドライバ作者): 6〜7 Mbps。260KB の JPEG で 0.47 秒(接続が温まっている時)〜1.70 秒(冷えている時)。
- **ISP チューニング**: `sensors/imx708/cfg/imx708_default.json`。CCM は校正値ではなく控えめな初期値。
- **ライセンス**: ドライバは Apache-2.0。
- **Wi-Fi の仕組み**: P4 自体に無線は無い。オンボードの ESP32-C6 を SDIO 経由で `esp_hosted` + `esp_wifi_remote` で使う。C6 側ファームと P4 側 `esp_hosted` のバージョン不整合で繋がらないことがある。
- **チップリビジョン**: 新しいリビジョン (v3.x) の P4 では対象リビジョン設定を合わせないと起動しないことがある。起動ログの `chip revision` を確認する。
- **FPC ケーブル**: カメラ側は 15pin (1.0mm ピッチ)。ボード側が 22pin (0.5mm) か 15pin かは**未確認**。付属の 15-22pin / 15-15pin の両ケーブルで対応できる。端子面の向きに注意(逆だと認識しない)。

## 4. 現在のファイルの状態

| ファイル | 状態 |
|---|---|
| `platformio.ini` | 完成(未ビルド)。`platform` URL の `stable` は動作確認後にリリース番号へ固定する |
| `CMakeLists.txt` / `src/CMakeLists.txt` | 完成 |
| `src/idf_component.yml` | バージョンは `*`。動作確認後に固定する |
| `sdkconfig.defaults` | **TODO あり**: IMX708 / DW9807 / esp_hosted の `CONFIG_` 名が未確定(推測で書かないこと) |
| `partitions.csv` | 32MB 用。OTA が必要になったら ota_0/ota_1 に変更する |
| `src/main.c` | 起動確認だけ(チップ情報・PSRAM 表示) |
| `src/secrets.example.h` | Wi-Fi / MQTT / アップロード先の定数 |
| `pc/server.py` | FastAPI の最小実装(アップロード受信・MQTT 経由の撮影コマンド) |

## 5. ロードマップ(次にやること)

ハードウェアが届いたら、この順で 1 段ずつ確認する。

1. **ビルドと起動確認**: `pio run -t upload && pio device monitor`。チップリビジョン、PSRAM 32MB、Flash 32MB が表示されることを確認する
2. **Wi-Fi**: Waveshare の ESP-IDF サンプルから esp_hosted の SDIO 設定を写して STA 接続し、IP を取得する
   - 参考: https://docs.waveshare.com/ESP32-P4-WIFI6/ESP-IDF
   - 確定した `CONFIG_ESP_HOSTED_*` を `sdkconfig.defaults` に書き戻す
3. **カメラ検出**: ドライバ付属の I2C スキャンのサンプルで、IMX708 (0x1A 付近) と DW9807 (0x0C) が見えることを確認する
   - `pio run -t menuconfig` → Component config → Camera Sensor (IMX add-on) → Support IMX708、Camera Motor (IMX add-on) → DW9807
   - 確定した `CONFIG_` 名を `sdkconfig.defaults` に書き戻す
4. **静止画**: ドライバのサンプルを元に `camera.c` を作る。`esp_video` で取得し、HW JPEG エンコードする
5. **HTTP**: `http.c` に `/capture` と `/status` を作り、ブラウザで JPEG が見えるか確認する。その後 `/stream` (MJPEG) を追加する
6. **MQTT**: `mqtt.c` で `cam/<id>/cmd` を購読し、`capture` を受けたら撮影して `upload.c` から PC の `/upload` へ POST する。LWT で `status` を offline にする
7. **PC 側**: `pc/server.py` を拡張して Web UI(撮影ボタン、最新画像、設定フォーム)を作る
8. **仕上げ**: platform とコンポーネントのバージョン固定、OTA、AF・露出の API 化、H.264 配信の検討

## 6. 引き継ぎ先 LLM への注意

- **推測で `CONFIG_` 名や API 名を書かない。** `esp_cam_sensor_imx` は新しいコミュニティ製コンポーネントで、学習データに無い可能性が高い。コンポーネントの README とサンプル (`managed_components/` 以下) を必ず読んでから書く。
- ESP32-P4、esp_video、esp_hosted は API の変更が速い。IDF のバージョンを確認してからドキュメントを参照する。
- 詰まったら、まず **PlatformIO を使わず ESP-IDF 単体**(`idf.py`)でドライバのサンプルが動くかを確認して切り分ける。ドライバ作者の検証は IDF 直接で行われている。
- ユーザーは日本語話者。電子工作・3D プリント・ネットワーク設定の経験があり、作業をブログで記録している。
- 返答や資料は日本語で書く。

## 7. 参考リンク

- Waveshare ESP32-P4-WIFI6: https://docs.waveshare.com/ESP32-P4-WIFI6
- esp_cam_sensor_imx: https://components.espressif.com/components/mushbraindave/esp_cam_sensor_imx
- esp32-p4-imx-camera (GitHub): https://github.com/mushBrainDave/esp32-p4-imx-camera
- pioarduino: https://github.com/pioarduino/platform-espressif32
- PlatformIO Community (P4 の設定): https://community.platformio.org/t/esp32-p4-and-platformio/31666
- FabScene 記事: https://fabscene.com/new/make/nfc-cassette-tape-display-home-assistant/
- nWorld 記事 (Arduino + OV5647、FPC・リビジョンの注意): https://firtel.blogspot.com/2026/09/esp32-p4-raspberry-pi-camera.html
- カメラ購入候補: https://www.switch-science.com/products/9933
