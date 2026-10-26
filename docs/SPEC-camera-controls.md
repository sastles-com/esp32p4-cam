# 仕様書: シャッター時間・明るさ・ピントの手動操作 (Web UI)

- 作成: 2026-10-13
- 対象: この仕様を実装するエージェント / 開発者
- 前提資料: [../HANDOFF.md](../HANDOFF.md)。特に「6. 引き継ぎ先 LLM への注意」を守ること

## 0. 目的と範囲

PC のブラウザ UI (`pc/camtool.py serve`) から、IMX708 の露出とピントを **Auto / Manual で切り替え**、Manual では次のスライダで操作できるようにする。

| スライダ | 中身 |
|---|---|
| シャッター時間 | センサーの露出時間 (µs)。`V4L2_CID_EXPOSURE` (単位はライン数) |
| 明るさ | センサーのアナログゲイン。`V4L2_CID_GAIN` (メニューの番号) |
| ピント | VCM (DW9807) の位置。`V4L2_CID_FOCUS_ABSOLUTE` (0〜1023 の DAC 値。大きいほど近距離) |

露出には **シャッター優先** も用意する。シャッター時間だけ手動で決め、明るさ (ゲイン) は自動で合わせるモード。

操作中は、既存の低解像度 MJPEG プレビュー (`:81/stream.mjpg`) で結果を見ながら調整する。撮影は 1920×1080 の JPEG (既定の品質 100)。

設定はボードの FS 上の `config.json` に保存し、再起動しても保持する (§3.6)。

**範囲外**(今回は作らない)
- ホワイトバランス
- 1 フレーム時間 (28fps で約 35ms) を超える長時間露光(フレーム長の変更が必要なため)
- 常時追従の AF(Auto ピントは「1 回合わせる」方式。§3.3)
- フル Auto 時の露出補正 (EV)。AE の目標輝度を実行時に変える API が無い
- `esp32p4-cam` 本体 (PlatformIO) への移植、MQTT
- 動画 (② AF 動画、③ MF 動画は別のアプリとして後で作る)

## 1. 作業場所

### 1.1 ボード側: 新しいサンプルを作る

`esp32-p4-imx-camera` の、**ローカルで変更済みの** `imx708_wifi_snapshot` を丸ごとコピーして新しいサンプルを作る。元のサンプルは、閉ループ AF が動く基準として変更しない。

```
コピー元: /Users/katano/work/ESP32-P4/esp32-p4-imx-camera/components/esp_cam_sensor_imx/examples/imx708_wifi_snapshot
コピー先: 同じ親ディレクトリの imx708_wifi_control
```

- `build/`、`managed_components/`、`sdkconfig` はコピーしない
- `components/imx_wifi/` と、gitignore 済みの Wi-Fi 認証情報 `wifi_credentials.h` はコピーする
- `CMakeLists.txt` の `project(imx708_wifi_snapshot)` を `project(imx708_wifi_control)` に変更する
- `main/imx708_wifi_snapshot_main.c` を `main/imx708_wifi_control_main.c` に改名し、`main/CMakeLists.txt` の `SRCS` も合わせる
- `main/idf_component.yml` の `override_path: "../../.."` は、同じ深さなのでそのままでよい

### 1.2 PC 側

`/Users/katano/work/ESP32-P4/esp32p4-cam/pc/camtool.py` を拡張する。新しいファイルは作らない。

### 1.3 ビルドと書き込み

```sh
source /Users/katano/work/ESP32-P4/esp32-p4-imx-camera/idfenv.sh   # idf 関数が定義される (IDF 5.4.0)
cd .../examples/imx708_wifi_control
rm -f sdkconfig          # sdkconfig.defaults は sdkconfig が無いときしか読まれない
idf set-target esp32p4
idf build
idf -p /dev/cu.usbmodem5B910635451 flash
```

- `idf monitor` は対話式なので、エージェントからは使えない。シリアルログは pyserial などで、タイムアウトを付けて読む
- ボードと PC は同じネットワークに置く。コワーキングの WORKSTYLING はクライアント分離で PC からボードに届かないので、iPhone テザリング (SSID ktnyso) を使う

## 2. 確認済みの事実 (API 名・場所)

推測で名前を書かないこと。以下はソースで確認済み。パスは `.../imx708_wifi_snapshot/` からの相対パス。

| 用途 | 名前 | 場所 |
|---|---|---|
| AE (AGC) の ON/OFF | `esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE / _DISABLE)`, `..._get_agc_status()` | `managed_components/espressif__esp_video/include/esp_video_isp_pipeline.h:33-57` |
| AE が使う露出範囲 (µs) | `esp_video_isp_pipeline_set_agc_min_exposure(us)`, `..._max_exposure(us)` と、それぞれの get | 同 `:127-164`。実装は `src/esp_video_isp_pipeline.c:1840-1951` |
| 範囲チェック | `isp_check_and_align_exposure()`。センサーの範囲外は `ESP_ERR_INVALID_ARG`。step に切り捨てる。**範囲は `esp_video_init()` 時点の値で固定** | 同 `.c:1840` |
| 露出・ゲインの書き込み | `ctrl_get_user()` / `ctrl_set_user()` (USER クラス) | `../imx708_snapshot/main/imx708_snapshot_main.c:772, 784` |
| ゲインのメニュー値 | `gain_menu_value()` (`VIDIOC_QUERYMENU`。1000 = 1.00 倍) | 同 `:800` |
| AE を切って露出を書く手順 | `apply_exposure_bias()`。step に揃えてから範囲を確認する。読み戻しで確認する | 同 `:828` |
| ピントの読み書き | `focus_get()` / `focus_set()` (`V4L2_CID_FOCUS_ABSOLUTE`、**CAMERA クラス**) | 同 `:557, 569` |
| 鮮鋭度 | `centre_sharpness()` (中央帯、緑チャネルの横方向勾配の平均) | 同 `:592` |
| レンズ移動後に捨てるフレーム数 | `FOCUS_SETTLE_FRAMES 4` | 同 `:183` |
| 1 ラインの時間 | `IMX708_TLINE_NS 13361` (ns) | `components/esp_cam_sensor_imx/sensors/imx708/imx708.c:36` |
| フレーム取得 | `capture_task()` が DQBUF を独占し、最新フレームを `s_frame` に `s_frame_lock` で公開する。`s_frame_seq` が毎フレーム増える | `main/imx708_wifi_snapshot_main.c:128, 184` |
| HTTP サーバー | `start_server()`。`HTTPD_DEFAULT_CONFIG()`、URI は 5 個 | 同 `:586` |

### 2.1 知っておくべき挙動

- **AF を止める実行時 API は無い。** `CONFIG_ESP_VIDEO_ISP_PIPELINE_CONTROL_CAMERA_MOTOR=y` の間は、ISP が AF の結果を `config_motor_position()` で書き続ける。そのため、こちらが書いたピント位置は上書きされる(`imx708_snapshot` の `#error`、`:246` を参照)
- IPA の AF は山登り方式。合焦後もシーンが変わると再探索する (`imx708_default.json` の `af` ブロック)
- AE の設定 (`imx708_default.json` の `agc`): `anti_flicker` が `"mode": "full", "ac_freq": 60`、ゲインの上限が `gain.max = 16`
- **USER クラスと CAMERA クラスの食い違い**: `imx708_snapshot` は露出とゲインを USER クラスで読み書きして実機で動いている。一方 `esp_video_isp_pipeline.c` は CAMERA クラスで書いている。**`ctrl_get_user`/`ctrl_set_user` をそのまま流用し、書いた後は必ず読み戻して確認する。** クラスを間違えるとエラーにならず、黙って何も起きない
- 露出は step の倍数でないと拒否される。step に揃えてから範囲チェックする(`apply_exposure_bias` のコメントを参照)

## 3. ボード側の仕様

### 3.1 sdkconfig.defaults の変更

```
CONFIG_ESP_VIDEO_ISP_PIPELINE_CONTROL_CAMERA_MOTOR=n   # ← y から変更。ISP の AF がレンズを動かさなくなる
CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER=y      # 維持。無いと VCM 自体が動かない
CONFIG_ESP_IPA_AF_ALGORITHM=y                          # 維持。JSON の af ブロックがこれを要求する。外すと AE/AWB も失われる
```

ファイル中のコメントも、上の理由に書き換える。ビルド後に `sdkconfig` で 3 つの値を確認する。

**影響**: 起動直後のレンズは `CONFIG_CAM_MOTOR_DW9807_INIT_POS=512` に止まったまま、ピントが合わない。そのため起動時に 1 回 AF を走らせる (§3.3)。

### 3.2 露出の状態

露出は 2 つの設定で決まる。

| exposure_mode | gain_mode | 名前 | 動作 |
|---|---|---|---|
| `auto` | (無視) | フル Auto | AGC を ON。min/max は起動時の値 |
| `manual` | `auto` | シャッター優先 | 露出を固定し、ゲインだけ自動で合わせる。実現方法は §3.2.1 |
| `manual` | `manual` | フル Manual | AGC を OFF。露出とゲインを両方書く |

**起動時**: AE が動き出した後に `get_agc_min_exposure` / `get_agc_max_exposure` で範囲を保存する (`s_boot_min_us`, `s_boot_max_us`)。Auto に戻すときは必ずこの範囲に戻す。戻さないと、Auto にしても露出が固定されたままになる。

**切り替えの手順**

- 何か → フル Auto: 自前のゲインループ(方式 B)があれば止める → min/max を起動時の値に戻す → AGC を ON
- 何か → フル Manual: ゲインループを止める → AGC を OFF → ゲインを書く → 露出を書く → 読み戻す
- 何か → シャッター優先: §3.2.1 の方式による
- **Manual に入るとき、値が指定されていなければ、今 AE が使っている露出とゲインを読み取ってそのまま書く。** 切り替えの瞬間に明るさが跳ばないようにするため

**シャッター時間とプレビューのコマ数**: 露出はフレーム長 (28fps で約 35.7ms) の内側で変わるだけなので、シャッター時間を長くしてもプレビューのコマ数は落ちない。上限 (約 35ms) までは、明るさだけが変わる。

#### 3.2.1 シャッター優先の実現方法 (Phase 0 で決める)

**方式 A**(第一候補、未検証): AGC を ON のまま、`set_agc_min_exposure(t)` と `set_agc_max_exposure(t)` に同じ値 t を入れる。露出の動ける幅がなくなるので、AE はゲインだけで明るさを合わせるはず。

ただし esp_ipa はソースの無いビルド済みライブラリで、min = max を受け付けるかは確認されていない。anti-flicker (60Hz、full) が露出を 1/120 s (8333µs) の倍数に寄せて、固定値を上書きする可能性もある。

**方式 B**(代替。実機で実績のある制御だけを使う): AGC を OFF にして露出を書き、ゲインはアプリ側の小さなループで合わせる。

- 専用タスクを 1 つ作る。約 200ms ごとに `s_frame` の平均輝度を測る。RGB565 の緑 6bit を縦横 8 画素おきに間引き、`s_frame_lock` を持って読む
- 目標は 0〜63 スケールで 25 前後(JSON の `luma_adjust.target: 100` を 0〜255 から換算した値)。外れていたらゲインのメニュー番号を ±1 する
- 不感帯は ±10%。ゲイン変更後は 4 フレームを待ってから再測定する。上限は `gain.max = 16` 倍に相当する番号
- 方式 B なら目標輝度をずらせる。将来 EV 補正スライダを付けるときは、この目標値を変えればよい(今回は作らない)

**Phase 0 の合否** (§5): 方式 A で t = 1000µs、8333µs、20000µs を順に試す。いずれも約 30 フレームの間、`V4L2_CID_EXPOSURE` を毎フレーム読み、t のライン数(step に揃えた値)から動かないこと。加えて、手で覆う・照明を当てるなどして明るさを変えたとき、`V4L2_CID_GAIN` が追従すること。3 つとも満たせば A を採用し、1 つでも満たさなければ B を採用する。結果は HANDOFF.md に記録する。

### 3.3 ピント

| focus_mode | 動作 |
|---|---|
| `manual` | `focus_pos` をそのまま `focus_set()` で書く |
| `auto` | 1 回だけ AF を走らせ、最も鮮鋭な位置に止める。その後は動かさない |

AF が走るのは次の 3 つの場合。

1. 起動時: Wi-Fi 接続後、AE が落ち着くのを 2 秒待ってから。ただし `config.json` の `focus_mode` が `manual` なら走らせず、保存された `focus_pos` に動かす (§3.6)
2. `focus_mode` を `auto` に切り替えたとき
3. `POST /af` を受けたとき

**AF タスク** (`af_task`。HTTP ハンドラの中では走らせない)
- 探索範囲は JSON で実測済みの 448〜960。粗い探索は 64 刻みで 9 点、細かい探索は最良点の ±64 を 16 刻みで 9 点
- 各点で `focus_set()` → `s_frame_seq` が `FOCUS_SETTLE_FRAMES + 1` 進むまで `vTaskDelay(1)` で待つ → `s_frame_lock` を持って `centre_sharpness(s_frame, s_w, s_h)` で評価する
- **DQBUF は自分で呼ばない。** `capture_task` が独占している
- 状態 `af_state` は `idle` / `running` / `done` / `failed`。最後の位置とスコアを残す
- 走行中に `POST /af` が来たら 409 を返す。走行中に `focus_mode=manual` が来たら探索を打ち切り、指定位置を書く
- 1 点あたり約 0.2 秒、合計 4 秒前後を見込む
- **AF の実行中も MJPEG プレビューと `/snapshot.jpg` を止めない。** AF タスクは `capture_task` と HTTP サーバーから独立しており、評価のときに `s_frame_lock` を数十 ms 持つだけ。レンズが動く様子はプレビューにそのまま映ってよい

### 3.4 排他

- 制御用の ioctl (露出・ゲイン・ピント) とモード変数は、すべて新しいミューテックス `s_ctrl_lock` の内側で扱う
- ロックの順序は `s_ctrl_lock` → `s_frame_lock` → `s_jpeg_lock`
- AF タスクは評価のときだけ `s_frame_lock` を持つ。待っている間はどのロックも持たない
- `capture_task` が使う fd を共有してよい。open が `O_RDONLY` なので、`S_EXT_CTRLS` が通るかは Phase 1 の最初に確認する。通らなければ `O_RDWR` に変える

### 3.5 HTTP API (ポート 80、既存の main サーバーに追加)

`start_server()` で `cfg.max_uri_handlers = 16` に増やす(既定は 8。既存の 5 個に 4 個足すと足りない)。すべてのレスポンスに `Access-Control-Allow-Origin: *` と `Cache-Control: no-store` を付ける。JSON は `snprintf` で組み立てる(cJSON は不要)。

#### `GET /ctrl/caps`

```json
{"tline_ns":13361,
 "exposure_us":{"min":27,"max":35660,"step":27},
 "gain":{"min":0,"max":N,"x":[1.00,1.07, ...]},
 "focus":{"min":0,"max":1023,"scan_min":448,"scan_max":960},
 "shutter_priority":"A"}
```

- 数値は例。**すべて実行時に `VIDIOC_QUERY_EXT_CTRL` と `gain_menu_value()` で取得する。** 露出はライン数 × `tline_ns` / 1000 で µs に換算する
- `shutter_priority` は §3.2.1 で採用した方式 (`"A"` または `"B"`)。UI は表示するだけ

#### `GET /ctrl`

```json
{"exposure_mode":"auto","gain_mode":"auto","focus_mode":"auto",
 "exposure_us":16670,"exposure_lines":1248,"gain_index":12,"gain_x":2.31,
 "focus_pos":576,"af_state":"done","af_score":2140,"af_ms":3820,"frame_seq":10234}
```

- `exposure_*`、`gain_*`、`focus_pos` は**常に読み戻した実際の値**。Auto 中は AE が決めた値が見える

#### `POST /ctrl?key=value&...`

本文 (body) は使わず、クエリ文字列だけで受ける。指定されたキーだけを変更する。

| キー | 値 |
|---|---|
| `exposure_mode` | `auto` / `manual` |
| `gain_mode` | `auto` / `manual` (`exposure_mode=manual` のときだけ意味を持つ) |
| `exposure_us` | 整数。`caps.exposure_us` の範囲に収め、step に切り捨てる |
| `gain_index` | 整数。範囲に収める |
| `focus_mode` | `auto` / `manual` |
| `focus_pos` | 整数 0〜1023 |

- 成功したら `GET /ctrl` と同じ JSON を返す。値は読み戻した後の値
- 範囲外は 400 にせず、範囲に収めて適用する。収めたことはレスポンスの値でわかる
- 未知のキーや不正な文字列は 400
- 1 回のリクエストで複数のキーを受けたときは、mode → 値の順に適用する
- 書いた値が読み戻しと違ったら `ESP_LOGW` を出す

#### `POST /af`

- AF を開始し、すぐに `202 {"af_state":"running"}` を返す。走行中なら 409
- `focus_mode` は `auto` になる

#### `POST /config/reset`

- `config.json` を削除して、すべてのモードを Auto に戻す (§3.6)。`GET /ctrl` と同じ JSON を返す

#### 既存エンドポイント

`/`、`/snapshot.jpg`、`/snapshot.raw`、`/bench`、`/stats`、`:81/stream.mjpg` は変更しない。`index_handler` のページは手を入れなくてよい。

### 3.6 設定の保存 (`config.json`)

露出とピントの設定を、ボードの Flash 上の FS に `config.json` として保存する。再起動しても同じ設定で立ち上がる。

**FS**: IDF 標準の SPIFFS (`esp_spiffs`)。追加の依存は無い。`partitions.csv` に次の 1 行を足す (Flash は 32MB で余裕がある)。

```
storage,  data, spiffs,  ,        1M
```

- 起動時に `/cfg` にマウントする。`format_if_mount_failed = true`
- マウントに失敗しても、カメラは既定値 (すべて Auto) で動かす。ログに警告を出すだけにする

**中身** (例):

```json
{"version":1,
 "exposure_mode":"manual","gain_mode":"auto","exposure_us":4000,"gain_index":0,
 "focus_mode":"manual","focus_pos":576}
```

- 読み書きは IDF 同梱の cJSON (`json` コンポーネント) を使う
- 保存するのは「ユーザーが指定した値」。Auto 中に AE が決めた値は保存しない。ただし Manual のときの `exposure_us` / `gain_index` / `focus_pos` は保存する
- 読み込み時に、知らないキーは無視する。値が範囲外なら範囲に収める。`version` が違う、または JSON として壊れていれば、ファイルを無視して既定値で起動する

**書き込みのタイミング**:
- `POST /ctrl` で設定が変わったら、2 秒後に書く。その間に次の変更が来たら、タイマーを延長する (`esp_timer` の one-shot)。スライダを動かすたびに Flash に書かないため
- AF が完了したときは、`focus_mode` が `auto` なので保存しない。ピント位置は保存しない
- 書き込みは `config.json.tmp` に書いてから `rename` する。書き込み中に電源が落ちても、前の設定が残るようにする
- 書き込みはタイマーのコールバックではなく、専用の低優先度タスクで行う。コールバックからはタスクに通知だけを送る

**起動時の適用順**:
1. カメラが STREAMON した後、`config.json` を読む
2. 露出: §3.2 の切り替え手順で適用する。起動時の AE 範囲 (`s_boot_min_us`/`s_boot_max_us`) は、適用前に保存しておく
3. ピント: `manual` なら `focus_set(focus_pos)`。`auto` なら Wi-Fi 接続後に AF を走らせる (§3.3)

**`GET /ctrl` への追加**: `"config":"loaded" | "default" | "error"` を返す。起動時に読めたかどうかを UI に表示するため。

## 4. PC 側の仕様 (`pc/camtool.py`)

### 4.1 FastAPI の中継エンドポイント

ブラウザはボードを直接呼ばず、すべて camtool 経由にする(既存の `/api/shutter` と同じ方式)。`board` は既存の `HOST_RE` で検証する。

| camtool | ボード |
|---|---|
| `GET /api/ctrl/caps?board=` | `GET http://<board>/ctrl/caps` |
| `GET /api/ctrl?board=` | `GET http://<board>/ctrl` |
| `POST /api/ctrl?board=&<キー>=...` | `POST http://<board>/ctrl?<キー>=...`。許可するキーは §3.5 の 6 個だけで、それ以外は 400 |
| `POST /api/af?board=` | `POST http://<board>/af` |

- タイムアウトは 5 秒。接続できなければ 502、ボードが 4xx を返したらそのステータスで返す。既存の `shutter()` と同じ例外処理にする
- `urllib.request` を使う。依存は増やさない

### 4.2 UI (既存の右カラム `aside` に追加。プレビューの品質設定の下、シャッターボタンの上)

```
── 露出 ──────────────── [ Auto | Manual ]
 シャッター  1/250 s (4000 µs)   ●────────────   ← Manual のときだけ操作できる
 明るさ      ×2.31   [自動 ☑]    ──●──────────   ← 自動 ☑ = シャッター優先。☐ でスライダが効く
── ピント ─────────────── [ Auto | Manual ]
 位置        576                 ────●────────
 [ AF ]   合焦 576 (3.8 s)
```

- **露出の Auto/Manual** は `exposure_mode`、**明るさの「自動」チェック**は `gain_mode` (☑ = `auto`)、**ピントの Auto/Manual** は `focus_mode` に対応する
- **シャッターのスライダは対数目盛り。** 内部は 0〜1000 の整数で、`us = min × (max/min)^(v/1000)` と換算する。表示は 1/x s 形式 (1/x が整数に近ければ整数、1 秒未満のみ) と µs を併記する
- 明るさのスライダはゲインのメニュー番号を動かし、表示は `caps.gain.x[i]` 倍
- ピントのスライダは `caps.focus.min`〜`max`。初期の表示範囲は `scan_min`〜`max` でよい
- Auto のとき(または「自動」☑ のとき)、該当するスライダは無効表示にする。値は `GET /ctrl` を 1 秒ごとにポーリングして追従させ、AE や AF が今どこにいるかを見せる
- スライダの `input` イベントでは、最後の送信から 150ms 以上空けて送る(間引き)。`change` で最終値を必ず送る。送信中の次の値は、最新の 1 つだけ保持する
- Manual に切り替えたとき、スライダの初期値は直近の `GET /ctrl` の値にする(明るさが跳ばないように)
- **AF ボタン**: `POST /api/af` を送る。`af_state` が `running` の間は「合焦中…」と表示し、シャッターボタンを無効にする
- ページを開いたときとボードのアドレスを変えたときに `GET /api/ctrl/caps` を読む。失敗したら「ボードが制御 API に対応していません (imx708_wifi_control を書き込んでください)」と表示し、制御欄を無効にする
- 既存の配色トークン (`--acc` など) とダークモード対応に合わせる。スイッチは `<input type=checkbox>` を CSS でトグル表示にした程度でよい
- 制御の状態は localStorage に保存しない。ボード側の状態 (`config.json`) が正しい値
- **シャッター品質の既定値は 100** にする (今の UI は 95)
- 制御欄の下に「設定をリセット」の小さなリンクを置く。確認ダイアログの後に `POST /config/reset` を送る。camtool の中継にも `POST /api/config/reset?board=` を足す

### 4.3 CLI (任意)

`camtool.py ctrl --board <IP> [--exposure-us N] [--gain-index N] [--focus N] [--auto]` があると試験が楽になる。必須ではない。

## 5. 実装の順序と合否条件

各 Phase が終わるたびに、結果(数値・ログ)をユーザーに報告する。

**Phase 0 — 下準備とシャッター優先の方式決定**
1. §1.1 のとおり複製し、§3.1 の sdkconfig にしてビルド、書き込み。既存の `/snapshot.jpg` が撮れ、ピントが 512 のまま固定であることを確認する
2. 一時的なコードで §3.2.1 の試験を行い、A か B を決める。試験コードは Phase 1 の前に消す

**Phase 1 — ボードの API** (curl で確認)
- `curl -s http://<IP>/ctrl/caps` の範囲が妥当であること(露出の max が約 35ms、ゲインの x[0] が 1.00 付近)
- `curl -s -X POST 'http://<IP>/ctrl?exposure_mode=manual&gain_mode=manual&exposure_us=2000&gain_index=0'` の後、`GET /ctrl` の値が 10 秒間変わらないこと。`/snapshot.jpg` が暗くなること
- `exposure_us=20000` に変えると明らかに明るくなること
- シャッター優先: `exposure_us` を固定したまま明るさを変えると、`gain_index` だけが動くこと
- `exposure_mode=auto` に戻すと露出が動き出すこと。`get_agc_min/max` が起動時の値に戻っていることをログで確認する
- `focus_mode=manual&focus_pos=448` と `=960` でプレビューのボケ方が変わり、10 秒経っても位置が戻らないこと
- `POST /af` で約 5 秒以内に `af_state=done` になり、約 2m 先の被写体で `focus_pos` がおよそ 550〜650 になること(JSON の実測では 576)
- AF の実行中も `/snapshot.jpg` と MJPEG が止まらないこと
- 設定の保存: Manual の露出とピントを設定して 3 秒以上待ち、ボードを再起動する。起動後の `GET /ctrl` が同じモードと値で、`"config":"loaded"` になっていること。ピントが Manual なら、起動時に AF が走らないこと
- `POST /config/reset` の後に再起動すると、すべて Auto で起動し、`"config":"default"` になること
- 壊れた `config.json` (例: 中身を `{`) でも、既定値で起動すること。この試験は一時的なコードかシリアル経由で行ってよい

**Phase 2 — PC の UI**
- §4.2 の操作がすべてブラウザで動くこと
- スライダ (シャッター・明るさ・ピント) を動かしてから 1 秒以内に、プレビューに反映されること
- スライダを素早く動かしてもボード側の要求が詰まらず、プレビューが止まらないこと
- 旧ファーム (`imx708_wifi_snapshot`) につないだとき、制御欄が無効になり、撮影は今までどおりできること

**Phase 3 — 記録**
- HANDOFF.md に、新しいサンプルの場所、採用したシャッター優先の方式、Phase 0 の数値、既知の制約(§0 の範囲外)を追記する

## 6. 実装結果 (2026-10-15)

ボード側は `esp32-p4-imx-camera/components/esp_cam_sensor_imx/examples/imx708_wifi_control`、PC 側は `pc/camtool.py`。制御は `main/camctl.c` / `camctl.h` に分けた (②③の動画アプリで再利用するため)。フレームの扱いはアプリから 3 つのコールバックで渡す。

### 6.1 Phase 0: シャッター優先は方式 B を採用

方式 A (AGC の露出範囲を 1 点に固定) は不合格。露出は固定されるが、AGC はゲインを 1 回決めた後に調整しなくなる。

| シャッター | 方式 A のゲイン / 明るさ (Y) | 方式 B のゲイン / 明るさ (Y) |
|---|---|---|
| (Auto) | 4.0 倍 / 112〜128 | 同左 |
| 20000µs | 13.5 倍のまま / 181 | 6.0 倍 / 105 |
| 8333µs | 13.5 倍のまま / 116 | 16 倍 / 111 |
| 2000〜4000µs | 16 倍 / 64 | 16 倍 / 63〜75 (ゲインの上限。室内ではこれ以上明るくできない) |

方式 B のゲインループの目標輝度は、固定値ではなく **Auto を抜ける瞬間の画像の平均輝度** にした。シャッター優先に切り替えても、明るさが Auto と同じに保たれる。起動時から直接シャッター優先で始まるときだけ、既定値 110 を使う。収束は約 1 秒。`POST /ctrl?sp=A|B` で、書き込み直さずに方式を切り替えられる (試験用)。

### 6.2 仕様から変えた点と、見つかった問題

- **AE の保持範囲を狭めた** (`ipa/imx708_control.json`)
  - 共有の `imx708_default.json` は変更していない。このサンプル専用のコピーを作り、`CMakeLists.txt` でそれを指すようにした
  - 変更点は `agc.luma_adjust` のみ: `target_low/target/target_high` を `50/100/150` → `52/60/68` にした
  - 理由: 明るさがこの範囲に入っている間、AE は露出もゲインも変えない (esp_ipa README)。50〜150 は約 1.6 段と広く、AE が落ち着く明るさが近づく方向で変わっていた。起動時 (暗い側から) は Y=133、手動から戻したとき (明るい側から) は Y=188
  - 変更後は、起動時が Y=112〜116、手動から戻したときが Y=128〜131 に落ち着く
- **AE を止める・再開するときの手順**
  - ISP パイプラインは、自分が書いた露出とゲインだけを「現在値」として覚えている
  - そのため、AE を止める直前の値を記録し、再開する**前**にその値をセンサーへ書き戻す (`agc_set()`)
  - 書き戻しを再開の後にすると、AE とセンサーの値がずれたままになった
- **AF の待ち方**
  - このアプリがフレームを公開するのは毎秒 14 枚で、センサー (28fps) の 1 枚おき。4MB のコピーが追いつかないため
  - 「公開フレームを 5 枚待つ」だと 1 点あたり 360ms かかったので、「150ms 待ってから新しいフレームを 1 枚」に変えた
  - 細かい探索は中心 ±48 を 16 刻みで行い、中心は粗い探索の結果を使う (全 15 点)
  - 所要時間はプレビューなしで 3.2 秒、MJPEG を流しながらだと 5.6 秒 (480×264 でも、UI の既定の 960×536・品質 50 でも同じ)。§5 の「5 秒以内」はプレビュー中だと満たさない
- **MJPEG プレビューの縮小が動いていなかった** (元の `imx708_wifi_snapshot` からある不具合)
  - PPA は出力バッファの位置とサイズを L2 キャッシュの行 (このビルドでは 128 バイト) に揃える必要があるが、64 バイトに揃えていた
  - `CONFIG_CACHE_L2_CACHE_LINE_SIZE` に揃えるよう直した。`imx708_wifi_snapshot` 側は未修正
- **画像が強く緑がかる** (U≈85、V≈123)
  - 元の `imx708_wifi_snapshot` でも同じ値だったので、今回の変更ではない
  - 原因は IPA の AWB の設定 (§6.4)。アプリ側で WB を持つことで解消した
- 起動時の AE の露出範囲は 8333〜33332µs。下限はちらつき対策 (60Hz、1/120 秒) による

### 6.3 合否 (§5)

| 項目 | 結果 |
|---|---|
| Phase 0: sdkconfig、ピントが 512 で固定、シャッター優先の判定 | 合格 (方式 B を採用) |
| Phase 1: フル Manual の値が 10 秒保持される、明るさの変化 | 合格 |
| Phase 1: シャッター優先でゲインだけが動く | 合格 (方式 B) |
| Phase 1: Auto への復帰 | 合格 (§6.2 の修正後) |
| Phase 1: ピント 448/960 が 10 秒保持される | 合格 |
| Phase 1: AF が 5 秒以内 | プレビューなし 3.2 秒は合格。MJPEG を流しながらだと 5.6 秒 |
| Phase 1: AF 中の 2 回目の要求が 409、AF 中も MJPEG が止まらない | 合格 (12 秒で 82 フレーム、最長の間隔 0.36 秒) |
| Phase 1: 設定の保存・再起動後の復元・リセット | 合格。壊れた `config.json` の試験は未実施 |
| Phase 2: camtool の中継 API、品質 100 の撮影 | 合格 (1920×1080、1.48MB、1.9 秒) |
| Phase 2: ブラウザでの操作感、プレビューへの反映が 1 秒以内 | 未確認 (人の目で確認が必要) |

### 6.4 ホワイトバランス (2026-10-15 追加)

当初は範囲外だったが、ユーザーの要望で追加した。

- **方式**: IPA の AWB には、実行中に止める API が無い。AWB が動いている限り、アプリが書いた WB は上書きされる。そこで専用 JSON (`ipa/imx708_control.json`) から `"awb"` の項目を外し、`camctl` が ISP の赤・青のゲインを書く
  - 書き込み先は ISP デバイス `/dev/video20` で、`camctl` が 2 つ目のハンドルとして開く (esp_video は参照数を数えるので問題ない)
  - 制御は `V4L2_CID_RED_BALANCE` / `V4L2_CID_BLUE_BALANCE` (USER クラス、1000 = 1.0 倍)
  - このチップと IDF の組み合わせには専用の WB 回路 (WBG) が無いので、esp_video はゲインを CCM の赤・青の列に掛け合わせる。ACC が CCM を更新しても、掛け合わせは残る
  - ACC の CCM は色温度 0 の単位行列 1 件だけで、AWB の色温度推定に依存していない。AWB を外しても他への影響は無い
- **緑かぶりの原因**: 元の AWB 設定は `min_red/blue_gain_step: 0.5` (README の例は 0.034) で、ほとんど補正していなかった。WB を 1.0/1.0 にすると、以前と同じ緑 (U=88、V=123) になる
- **モード**
  - **Auto**: 画面全体のグレーワールド方式。300ms ごとに、R と B の平均を G に近づける。明るさの計測は γ 補正後なので、1 回の補正は比の 1.5 乗にとどめ、行き過ぎないようにしている
  - **ワンプッシュ** (`POST /wb/onepush`): 中央 1/4 の範囲が無彩色になるまで繰り返し (最大 15 回)、その後は Manual として値を保持する
  - **Manual**: 赤・青のゲインを 0.30〜3.999 倍で直接指定する
- **API**: `/ctrl` に `wb_mode`、`wb_red`、`wb_blue` (1000 = 1.0 倍) と `wb_state` を追加した。`caps.wb` は `{min, max}` を返す。ISP デバイスを開けなかった場合は `null`
- **設定の保存**: `wb_mode`、`wb_red`、`wb_blue` を `config.json` に保存する。`/config/reset` で Auto に戻る
- **UI**: ホワイトバランスの Auto/Manual スイッチ、赤・青のスライダ、「白を合わせる」ボタン。プレビューに、ワンプッシュで見る中央の範囲を点線の枠で表示する
- **実測** (同じ室内の場面)

| 状態 | 赤 / 青のゲイン | U / V (128 = 無彩色) |
|---|---|---|
| 以前 (IPA の AWB) | — | 85 / 123 |
| Auto (グレーワールド) | 1.23 / 2.61 | 125 / 127 |
| ワンプッシュ (0.7 秒で収束) | 1.32 / 2.34 | 117 / 132 (中央の物を基準にしたため、画面全体はやや暖色) |
| Manual 赤 3.0 / 青 1.0 | — | 69 / 181 |

- **制約**: グレーワールドは、画面の色が一色に偏る場面では引っ張られる。その場合はワンプッシュを使う。センサーの校正データが無いので、色温度 (K) の目盛りは付けていない

## 7. 注意

- **元の `imx708_wifi_snapshot` と `imx708_snapshot` は変更しない。** 関数は新しいサンプルにコピーして使う
- `sensors/imx708/cfg/imx708_default.json` も変更しない。他のサンプルと共有している (チューニングを変えるときは、§6.2 のように専用のコピーを作る)
- 書き込みとリセットはボードを使うので、実行前にユーザーに一言伝える
- 推測で API 名や `CONFIG_` 名を書かない。§2 に無いものは、ソースで確かめてから使う
