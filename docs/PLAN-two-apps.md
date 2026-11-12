# 2アプリの立ち上げ計画（Wi-Fi 単体 HTML / USB 接続）

> 作成: 2026-11-11。この文書は計画の記録。下の「進捗」を最新の状態として読むこと。

## 進捗（2026-11-12 時点）
| 段階 | 状態 | メモ |
|---|---|---|
| P0 USB 配線の確認 | 一部済 | ネイティブ USB は **MX1.25 4ピン**（シルク `V D- D+ G`）。P4 のどのピンかは回路図未確認。ケーブルの色とピン順は実物で確認が必要 |
| P0 SoftAP のスパイク | 未 | |
| P1 内蔵 UI（STA） | 済（サーバー側まで） | `common/webui/index.html`、`wifi-app/firmware`。実機で `/` と `/ctrl/caps` の応答を確認。**ブラウザでの操作感は未確認** |
| P2 SoftAP + Wi-Fi 設定画面 | 未 | |
| 共通部 `common/camcore` | 済 | wifi-app から切り出し（`camcore_init/start/serve`）。wifi-app で回帰確認済み |
| P3 usb-app 経路A（シリアル） | 済（実機・curl で確認） | UART 上の TCP トンネル + PC の FastAPI（`usb-app/README.md`）。q90 静止画 約 1.3 秒、プレビュー約 7 fps。**ブラウザでの操作感は未確認** |
| P4 usb-app 経路B（USB NCM） | 未 | 先にケーブルと配線の確認 |
| 画像の反転（左右・上下） | 済（API で確認） | センサーで反転し、変更時はボードを再起動。色も確認済み。**画面のチェックボックスは未操作** |
| 物理ボタンの写真を画面に取り込む | 実装済み・**未確認** | 画面が `/shot/info` を見張って取り込む。HTTP 撮影で経路は確認。物理ボタンでは未確認 |

次に実機で確認すること（2026-11-12 時点、出先のため未確認）:
- ブラウザで usb-app を操作（プレビュー、露出/WB/AF、シャッター、反転のチェックボックス）。
- 画面を開いたまま物理ボタンを押し、写真が「撮影した写真」に出るか。
- 別の PC / スマホから `http://<PC の IP>:8080/` が開けるか。
- セレクタ（MCP23017）をつないで、照明スイッチと `light=1` の点灯。
- wifi-app を書き込み直して、camcore の変更（反転、照明スイッチ、ボタン取り込み）が同じく動くか。

実装で計画から変わった点:
- `camctl.c` は未知のキーを 400（`unknown key`）で拒否することを確認した（`camctl.c:881`）。
- `camserver/` と `wifi-app/` の `override_path` は、ディレクトリの深さに合わせて `../../../../esp32-p4-imx-camera/...` に直した。
- 新ボード（rev v3.2）は無線コプロセッサが **ESP32-C5** だったため、`sdkconfig.defaults.c5` を重ねてビルドする。
- usb-app 経路A は「ボードの HTTP サーバーをそのまま使い、UART に TCP のトンネルを通す」方式にした（API を二重に持たない）。
  Wi-Fi なしでも lwIP のループバックで 127.0.0.1 に届くが、既定のキューでは 12 kB/s しか出ない。
  `TCP_RECVMBOX_SIZE=47` などで 1.3 MB/s（`usb-app/firmware/sdkconfig.defaults` に理由）。
- ログはフレーム化せず、同じ UART にそのまま流す（フレームは 1 回の `uart_write_bytes` で出すので混ざらない）。

## Context
camserver（`esp32p4-cam/camserver/`）は実機で動作確認済み（ボタン撮影、照明、露出/WB/AF 制御、MJPEG、JPEG 取得）。これを「現状で完成」とし、次の2つを別プロジェクトとして立ち上げる。

1. **wifi-app**: P4 自身が HTML を持ち、SSID 接続（STA）で、プレビュー・カメラ設定・撮影を行う。SSID が無い／接続失敗のときは SoftAP に落ちる。MQTT は使わない。
2. **usb-app**: USB で PC とつなぎ、プレビュー・撮影・設定を行う。PC 側は REST または HTML。経路は「シリアル（今の CH343）」と「USB HS（ネイティブ USB）」の両方を試す。

確定した方針（ユーザー回答）: `esp32p4-cam/` の中にサブディレクトリで並べる。SoftAP は STA の代替。USB は両経路。

## 調査でわかったこと（再利用の根拠）
- ボードは既に API を一式持っている: `/snapshot.jpg`, `:81/stream.mjpg`, `/ctrl`, `/ctrl/caps`, `/af`, `/wb/onepush`, `/config/reset`, `/shot*`, `/io`（`camserver/firmware/main/{camserver_main,camctl,shot,ioexp}.c`）。
- `camserver/pc/static/index.html` は 14.5KB の単一ファイル（外部ライブラリなし、CDN なし）。PC 側 `server.py` の `/api/*` は、ほぼボード API の中継。つまり **`/api/...` → `/...`、`board` 指定 → `location.hostname`** への機械的な書き換えで、ボード内蔵 UI にできる。
- 中継でない `/api/shutter`（保存）とギャラリーはボードに相当物が無い。ブラウザが `/snapshot.jpg` を blob で取得してダウンロードさせるか、既存の `/shot` リング（4枚）を使う。
- SoftAP: `esp_wifi_remote`（`WIFI_RMT_SOFTAP_SUPPORT=y`）と esp_hosted のホスト側は AP/APSTA に対応している。`imx_wifi.c` は STA 専用なので、AP の netif・設定・DHCP の追加が必要。実機での動作は未検証。
- USB: IDF 5.5.5 に device 側の例がある（`examples/peripherals/usb/device/tusb_ncm`, `tusb_serial_device`）。`esp_tinyusb` はマネージドコンポーネントで、現在は未取得。**`eppp_link` は USB 転送を持たない**ので App 2 には使えない。
- 今の USB-C は CH343（UART 橋渡し）で、約 200KB/s。フル JPEG（約 500KB）は約 2.4 秒、プレビューは低解像度のみ。ネイティブ USB HS はこれより桁違いに速いが、**基板のどのコネクタが P4 の OTG に配線されているかを、リポジトリ内の資料で確認できていない**。

## 構成
```
esp32p4-cam/
├─ camserver/            現状版。凍結（参照用）。触らない
├─ common/
│   ├─ camcore/          camserver から切り出した共有コンポーネント
│   │                    camera_start / frame ops / JPEG / camctl / shot / ioexp / pins.h
│   └─ webui/            内蔵 HTML（embed）+ API 登録（/ctrl, /snapshot.jpg, /shot …）
├─ wifi-app/firmware/    STA + SoftAP フォールバック（imx_wifi を拡張）
└─ usb-app/
    ├─ firmware/         経路A: シリアル  経路B: USB NCM（切替は Kconfig）
    └─ pc/               経路A のブリッジ（REST、同じ HTML を配信）
```
- camserver は変更せず、共有部分は `common/` にコピー＋整理して切り出す（camserver を壊さないため）。
- ファームは HANDOFF の方針どおり、アプリごとに別（USB 書き込みで切替）。
- ビルド環境は v3.2 ボード用（IDF 5.5.5、`sdkconfig.defaults.c5`、esptool 5.4。`memory/p4-v3-build-flash.md`）。

## 段階（この順で進める）
**P0 スパイク（実装前の確認）**
- ネイティブ USB のコネクタと P4 の OTG 配線を、Waveshare の回路図／実機（`lsusb` 相当）で確認する。ここで経路B の可否が決まる。
- SoftAP を最小コードで起動し、MJPEG のスループットと安定性を測る。

**P1 共通部の切り出し + 内蔵 UI（wifi-app の STA 版）**
- `common/camcore` を作る。`camserver_main.c` の `camera_start()` と frame ops、`encode_frame_jpeg()` を引き継ぐ（`camserver_main.c` の関数をそのまま移す）。
- `common/webui`: `index.html` を `EMBED_TXTFILES` で埋め込み、`GET /` で配信。`/api/shutter` 相当はブラウザ側ダウンロードに置換。
- `max_uri_handlers` は camserver と同様にモジュール数から計算する。
- 注意: ポート 80 は `max_open_sockets=4`。MJPEG が :81 を占有するので、:80 の API は軽く保つ。

**P2 SoftAP フォールバック（wifi-app）**
- `imx_wifi.c` を拡張: 認証情報が無い／STA 失敗時に AP（SSID は固定名＋MAC 末尾、WPA2）を起動。DHCP は既定 192.168.4.1。
- UI に Wi-Fi 設定画面を追加し、SSID/パスワードを NVS に保存して再起動で STA に戻す（コンパイル時の `wifi_credentials.h` 依存をやめる）。

**P3 usb-app 経路A（シリアル）**
- ログと通信が同じ UART なので、通信中はログを止める（ログレベル下げ or フレーミング）。
- PC 側 `usb-app/pc` に、シリアルを REST に変換するブリッジ（FastAPI）を作り、同じ `webui` の HTML を配信する。プレビューは低解像度・低 fps、撮影はフルサイズ（約 2.5 秒）。

**P4 usb-app 経路B（USB NCM、P0 で可なら）**
- `esp_tinyusb` + NCM でボードを USB Ethernet にする。サブネットは SoftAP（192.168.4.x）と重ならない値にする（例 192.168.7.x）。
- ボードの内蔵 HTTP サーバーがそのまま USB 越しに使えるので、PC はブラウザで開くだけ（ブリッジ不要）。

## 検証
- P1: ブラウザで `http://<IP>/` を開き、プレビュー表示、露出/WB/AF の変更、撮影（ダウンロード）ができる。PC の `server.py` なしで動くこと。
- P2: 認証情報なしで起動 → AP が見える → スマホで接続 → UI が開く。SSID を保存して再起動 → STA で繋がる。
- P3: PC にブリッジを起動し、同じ UI で操作できる。フル撮影の所要時間を計測して記録する。
- P4: Mac に USB Ethernet が現れ、`curl http://192.168.7.1/ctrl` が通る。MJPEG の fps を計測する。
- 各段階で: 実機 v3.2 に書き込み、ボタン撮影と照明（`/io`）が従来どおり動くことを確認（回帰）。

## リスクと未確認事項
- 経路B の可否は P0 の配線確認次第。無ければ USB は経路A（約 200KB/s）のみになる。
- SoftAP のスループットは未測定。C5 経由での AP 動作は実機で確認が必要。
- 内部 RAM が逼迫している（esp_hosted の DMA キュー調整済み）。USB スタックと AP の追加で再調整が必要になる可能性。
- `camctl.c` が未知のキーを自分で拒否するかは未確認（PC 側の `CTRL_KEYS` は単なるホワイトリスト）。内蔵 UI 化の前に確認する。
