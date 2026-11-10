# FastAPI エンドポイント一覧

`pc/server.py` の実装に対応する。

共通事項:
- `board` クエリは省略可能。省略すると `--board` / `BOARD_HOST` の値を使う。
- ボードに届かないときは 502、アドレスが不正なときは 400。ボードが 4xx を返した場合は、そのステータスをそのまま返す。
- 対話的な API ドキュメントは `GET /docs`(OpenAPI)で見られる。

## 撮影

| メソッド | パス | パラメータ | 動作 |
|---|---|---|---|
| POST | `/api/shutter` | `board?`, `q`(JPEG品質 1–100, 既定 95) | ボードの `/snapshot.jpg` を取得し `pc/captures/YYYYmmdd_HHMMSS_mmm.jpg` に保存。`{file, bytes, seconds, width, height}` を返す |
| GET | `/api/preview.mjpg` | `board?`, `q`(既定 50), `n`(縮小率 16/12/8/4, 既定 8) | ボード `:81/stream.mjpg` の MJPEG を中継 |
| GET | `/api/captures` | なし | 保存済み JPEG のファイル名一覧(新しい順) |
| GET | `/captures/{file}` | なし | 保存済み画像を返す(静的配信) |

## カメラ設定

| メソッド | パス | パラメータ | 動作 |
|---|---|---|---|
| GET | `/api/status` | `board?` | `{reachable, config, ctrl}`。`config` は `loaded` / `default` / `error` で、保存設定が復元されたかを示す。ボードに届かなくても 200 で `reachable:false` を返す |
| GET | `/api/ctrl` | `board?` | 現在の露出・ゲイン・ピント・WB の状態 |
| GET | `/api/ctrl/caps` | `board?` | 各コントロールの範囲(シャッター時間、ゲイン段、ピント位置など) |
| POST | `/api/ctrl` | `board?` と下表のキー(複数可) | 設定を変更。変更の 2 秒後にボードの LittleFS へ保存される |
| POST | `/api/af` | `board?` | オートフォーカスを 1 回実行(ボード側は 202、実行中なら 409) |
| POST | `/api/wb/onepush` | `board?` | ワンプッシュ WB を実行 |
| POST | `/api/config/reset` | `board?` | `config.json` を削除し、全て Auto に戻す |

### `POST /api/ctrl` で指定できるキー

下記以外のキーを渡すと 400 になる。

| キー | 値 |
|---|---|
| `exposure_mode` | `auto` / `manual` |
| `gain_mode` | `auto` / `manual`(`auto` はシャッター優先) |
| `exposure_us` | シャッター時間(µs) |
| `gain_index` | ゲインのメニュー番号 |
| `focus_mode` | `auto` / `manual` |
| `focus_pos` | レンズ位置 |
| `wb_mode` | `auto` / `manual` |
| `wb_red`, `wb_blue` | WB の赤・青ゲイン |

## 画面・その他

| メソッド | パス | 動作 |
|---|---|---|
| GET | `/` | 操作 UI(`pc/static/index.html`) |
| GET | `/docs`, `/openapi.json` | FastAPI 標準の API ドキュメント |

## CLI(`pc/server.py`)

| コマンド | 動作 |
|---|---|
| `server.py [serve] --board IP [--host H] [--port P]` | サーバー起動(既定 `127.0.0.1:8080`) |
| `server.py shot --board IP -q 95` | UI なしで 1 枚撮影して保存 |
