# usb-app PC サーバー API 仕様

`usb-app/pc/server.py`（FastAPI）が LAN に公開する HTTP API。ボードを USB シリアルでつなぎ、
この PC のサーバーを通して、ブラウザや他のプログラムからカメラを操作する。

- 対象: usb-app ファーム（トンネル版 2）と、このリポジトリの `usb-app/pc`
- 最終更新: 2026-11-12

## 1. 概要

```
クライアント ──HTTP──▶ server.py (:8080) ──USB シリアルのトンネル──▶ ボードの HTTP サーバー (:80 / :81)
```

- サーバーが自分で処理するのは **`GET /` と `GET /stream.mjpg` の 2 つだけ**。それ以外のパスはすべて、
  メソッド・パス・クエリをそのままボードへ中継し、ボードの応答（ステータス、本体）を返す。
- 写真の保存はしない。静止画はクライアントが受け取って保存する。
- 認証はない。LAN 内で使う前提。CORS ヘッダーは付けない（同じオリジンの画面から使う）。
- ボード側の API は wifi-app と同じ（`common/camcore`）。wifi-app ではボード自身の `:80` と `:81` で、
  同じパスがそのまま使える（その場合 `/stream.mjpg` だけは `:81` で、閲覧者ごとに 1 本）。

### 起動

```sh
usb-app/pc/run.sh [--serial PORT] [--baud 2000000] [--host 0.0.0.0] [--port 8080] [--debug]
```

| オプション | 既定 | 意味 |
|---|---|---|
| `--serial` | 自動（CH343、VID 0x1A86） | ボードのシリアルポート |
| `--baud` | 2000000 | ファームの設定と同じ値にする |
| `--host` | `0.0.0.0` | 待ち受けアドレス。既定は全インターフェース |
| `--port` | 8080 | 待ち受けポート |
| `--debug` | なし | トンネルのチャンネルの開閉をログに出す |

起動するとボードがリセットされる（macOS ではポートを開くと DTR/RTS が動くため）。
ボードとつながると、コンソールに `[tunnel] board: usb-app tunnel 2` が出る。

## 2. 共通事項

### 2.1 エラー

| 状況 | ステータス | 本体 |
|---|---|---|
| ボードにつながっていない、応答がない（15 秒）、チャンネルが空かない（10 秒） | 503 | `{"error": "board: <理由>"}` |
| ボードが返したエラー | ボードのステータス | ボードの本体。多くは `{"error": "<理由>"}` |
| ボードに無いパス | 404 | `Nothing matches the given URI`（テキスト） |

ボードの JSON エラーは `{"error": "..."}` の形。画面（`common/webui`）は `error` を表示している。

### 2.2 同時実行と速さ

- ボードの `:80` へは同時に 3 本まで。4 本目以降は空くまで待つ（最大 10 秒）。
- 律速は USB シリアル（2 Mbaud で約 200 KB/s）。目安:

| 操作 | 時間 |
|---|---|
| `/ctrl` などの JSON | 0.02〜0.35 秒 |
| `/snapshot.jpg?q=90`（約 230 KB） | 約 1.3 秒 |
| `/snapshot.jpg?q=100`（約 1.6 MB） | 約 8 秒 |
| `/stream.mjpg` 960×536 q50 | 約 7 fps |

- 大きな転送（`/snapshot.jpg`、`/snapshot.raw`、`/shot.jpg`、`/bench` の GET）の間は、プレビューの
  上流を止めて帯域を譲る。終わると自動で再開する。

### 2.3 中継するヘッダー

ボードの応答から次のヘッダーだけを返す: `Content-Type`、`Content-Length`、`Cache-Control`、
`X-Frame-Size`、`X-Frame-Format`、`X-Shot-Id`。

### 2.4 ボードの再起動

向き（`hflip`・`vflip`）の変更と、向きが設定されている状態での `/config/reset` は、ボードを再起動する
（応答の約 0.8 秒後）。再起動の間（約 3 秒）は 503 になる。サーバーは自動でつながり直し、
`/stream.mjpg` の閲覧者には再起動後のフレームが続けて届く。

## 3. サーバー固有のエンドポイント

### `GET /`

Web 画面（`common/webui/index.html`）。プレビューの URL を `/stream.mjpg` に向けるため、
`<script>window.STREAM_URL="/stream.mjpg"</script>` を差し込んで返す。`Cache-Control: no-store`。

### `GET /stream.mjpg`

ライブプレビュー（Motion JPEG）。

| クエリ | 既定 | 意味 |
|---|---|---|
| `q` | 50 | JPEG 品質 1〜100 |
| `n` | 8 | サイズ。16 = 1920×1080、12 = 1440×792、8 = 960×536、4 = 480×264 |

- 応答: `multipart/x-mixed-replace; boundary=frame`。各パートは `Content-Type: image/jpeg` と
  `Content-Length` 付きの 1 枚の JPEG。終わりはない（クライアントが切るまで続く）。
- ボードからの流れは **常に 1 本** で、全閲覧者に同じフレームを配る。新しい閲覧者が別の `q`・`n` を
  指定すると、上流をその値でつなぎ直す（他の閲覧者の画質もそれに変わる）。
- 遅い閲覧者は古いフレームを飛ばす（遅れは溜まらない）。
- `n` が上の 4 つ以外だと、上流がつながらず、フレームが届かない（ボードは 400 を返す）。

## 4. ボードへ中継するエンドポイント

### 4.1 カメラの状態と設定

#### `GET /ctrl`

現在の状態。

```json
{
  "exposure_mode": "auto", "gain_mode": "auto", "focus_mode": "auto",
  "exposure_us": 33322, "exposure_lines": 2494,
  "gain_index": 6, "gain_x": 1.58,
  "focus_pos": 560, "af_state": "done", "af_score": 366, "af_ms": 3181,
  "wb_mode": "manual", "wb_red": 1820, "wb_blue": 1918, "wb_state": "idle",
  "hflip": false, "vflip": false,
  "shutter_priority": "B", "config": "loaded"
}
```

| フィールド | 意味 |
|---|---|
| `exposure_mode` / `gain_mode` / `focus_mode` / `wb_mode` | `auto` または `manual` |
| `exposure_us`, `exposure_lines` | 今のシャッター時間（µs、センサーの行数） |
| `gain_index`, `gain_x` | 今のゲイン（メニューの番号、倍率） |
| `focus_pos` | レンズ位置（DAC 値。大きいほど近く） |
| `af_state` | `idle` / `running` / `done` / `failed`。`af_score`、`af_ms` は最後の AF の鮮鋭度と所要時間 |
| `wb_red`, `wb_blue` | WB のゲイン（1000 = ×1.0） |
| `wb_state` | ワンプッシュ WB の状態。`idle` / `running` / `done` / `failed` |
| `hflip`, `vflip` | 左右反転、上下反転 |
| `shutter_priority` | シャッター優先の方式（`A` または `B`。通常は `B`） |
| `config` | 設定ファイルの状態。`loaded` / `default` / `error` |

#### `POST /ctrl?key=value&…`

指定したキーだけを変える。本体は使わず、クエリで渡す。応答は `GET /ctrl` と同じ形の、変更後の状態。
変更は約 2 秒後にボードの `config.json` に保存され、再起動しても残る。

| キー | 値 | 意味 |
|---|---|---|
| `exposure_mode` | `auto` / `manual` | 露出。manual にすると、値を省いた場合は今の値から始める |
| `exposure_us` | 整数（µs） | manual 時のシャッター時間。範囲は `/ctrl/caps` に合わせて丸める |
| `gain_mode` | `auto` / `manual` | 露出が manual のときのゲイン。auto = シャッター優先 |
| `gain_index` | 整数 | manual 時のゲイン（`/ctrl/caps` の `gain.min`〜`max`） |
| `focus_mode` | `auto` / `manual` | ピント。manual → auto で AF を 1 回走らせる |
| `focus_pos` | 整数 | manual 時のレンズ位置（`focus.min`〜`max`） |
| `wb_mode` | `auto` / `manual` | WB。auto はグレーワールド |
| `wb_red`, `wb_blue` | 300〜3999 | manual 時の WB ゲイン（1000 = ×1.0） |
| `sp` | `A` / `B` | シャッター優先の方式 |
| `hflip`, `vflip` | `0` / `1` | 左右反転、上下反転。**変えるとすぐ保存し、ボードを再起動する**（2.4） |

- 知らないキーが 1 つでもあると、何も変えずに 400 `{"error":"unknown key"}`。
- 値の形が違うと 400（例: `{"error":"focus_pos must be an integer"}`）。
- 例: `POST /ctrl?exposure_mode=manual&exposure_us=8000&gain_mode=auto`

#### `GET /ctrl/caps`

設定できる範囲。

```json
{
  "tline_ns": 13361,
  "exposure_us": {"min": 53, "max": 35059, "step": 27},
  "gain": {"min": 0, "max": 46, "x": [1.123, 1.189, …, 16.000]},
  "focus": {"min": 0, "max": 1023, "scan_min": 448, "scan_max": 960},
  "wb": {"min": 300, "max": 3999},
  "shutter_priority": "B"
}
```

- `gain.x` は `gain_index` ごとの倍率（`min` から順）。
- `focus.scan_min`〜`scan_max` は AF が探す範囲。
- `wb` が `null` なら WB の操作はできない。
- **カメラが無いと 404**（`/ctrl`、`/af` なども同じ）。画面はこれでカメラの有無を見分けている。

#### `POST /af`

AF を 1 回走らせる（ピントを auto にする）。`202 {"af_state":"running"}`。走っている間は
`409 {"error":"af already running"}`。結果は `GET /ctrl` の `af_state`・`focus_pos` で見る（数秒かかる）。

#### `POST /wb/onepush`

画面中央（縦横 25% の範囲）を白として WB を合わせる。`202 {"wb_state":"running"}`。
走っている間は 409。WB が使えないと 503。結果は `GET /ctrl` の `wb_state` と `wb_red`・`wb_blue`。

#### `POST /config/reset`

保存した設定（露出、ピント、WB、向き）を消し、すべて auto・反転なしに戻す。応答は戻した後の状態。
向きが設定されていた場合は、ボードが再起動する（2.4）。

### 4.2 静止画

#### `GET /snapshot.jpg?q=90`

最新のフレームを JPEG にして返す（1920×1080、4:2:2）。`q` は 1〜100（既定 90）。
`Content-Type: image/jpeg`。照明は操作しない。

#### `GET /snapshot.raw`

最新のフレームを RGB565 の生データで返す（1920×1080×2 = 約 4.1 MB、chunked）。
ヘッダー `X-Frame-Size: 1920x1080`、`X-Frame-Format: RGB565`。シリアルでは約 20 秒かかる。

### 4.3 照明付きの撮影（ボードに貯める）

ボタン（GPIO47）と同じ撮影を HTTP から行う。撮った JPEG はボードの PSRAM に **最新 4 枚** まで残り、
後から取り出す。照明を点けてから露出が落ち着くのを待って撮る。

#### `POST /shot?q=95&light=1`

| クエリ | 既定 | 意味 |
|---|---|---|
| `q` | 95 | JPEG 品質 1〜100 |
| `light` | 1 | `0` で照明を点けずに撮る |

`202 {"id": 7}` で受け付け、撮影は裏で進む。撮影待ちが詰まっていると `409 {"error":"shot queue full"}`、
カメラが無いと 503。

#### `GET /shot/info`

```json
{
  "boot": "8f812c68", "busy": false, "next": 8, "done": 7,
  "shots": [
    {"id": 6, "bytes": 512345, "light": true, "settle_ms": 420, "age_ms": 15300, "src": "button"},
    {"id": 7, "bytes": 498812, "light": true, "settle_ms": 380, "age_ms": 2100, "src": "http"}
  ]
}
```

| フィールド | 意味 |
|---|---|
| `boot` | 起動ごとに変わる値。変わったらボードが再起動した（貯めた写真は消えている） |
| `busy` | 撮影中 |
| `next` | 次に付く番号。`done` は最後に撮り終えた番号 |
| `shots` | 残っている写真（古い順）。`src` は `button` または `http`。`light` は照明が実際に点いたか（照明ボードが無ければ `light=1` でも `false`）。`settle_ms` は照明後に待った時間 |

カメラが無いときは、ボタンを押すと照明が 0.4 秒光るだけで写真は残らない（`done` は増える）。

#### `GET /shot.jpg?id=7`

貯めた写真を返す。`id` を省くと最新の 1 枚。ヘッダー `X-Shot-Id` に番号。無ければ 404。

### 4.4 照明（MCP23017）

#### `GET /io`

```json
{"present": true, "addr": 32, "out": 3, "light": true}
```

| フィールド | 意味 |
|---|---|
| `present` | 照明ボード（MCP23017）が見つかったか。起動時に 1 回だけ探す |
| `addr` | I2C アドレス（32 = 0x20） |
| `out` | ポート B の 8 本の出力（ビット 0 = OUT0） |
| `light` | 照明の出力（今は OUT0 と OUT1）がすべてオンか |

#### `POST /io?…`

次のどれか 1 つを指定する。応答は `GET /io` と同じ形。

| クエリ | 意味 |
|---|---|
| `light=0` / `light=1` | 照明の出力をまとめて消す / 点ける |
| `ch=0..7&on=0|1` | 出力を 1 本だけ |
| `out=0..255` | 8 本すべて |

照明ボードが無い・応答しないと `503 {"error":"MCP23017 not responding"}`。値が違うと 400。
照明を点けたまま `/shot` や `/snapshot.jpg` を撮ると、点いたまま撮る（撮影後も消えない）。

### 4.5 診断

#### `GET /stats`

最後の転送の計測値（テキスト、`key=value` の行）。

```
last=jpeg
bytes=232079
encode_ms=76
send_ms=68
mbit_s=27.2
frames_published=1234
heap_free=301919
psram_free=33550116
```

`send_ms` はボード内（127.0.0.1）への送信時間で、シリアルの転送時間は含まない。

#### `GET /bench`

カメラを通さない 8 MB のパターン（chunked）。経路の速さを測る。シリアルでは約 40 秒かかる。

## 5. 例

```sh
B=http://192.168.11.164:8080
curl $B/ctrl/caps
curl -X POST "$B/ctrl?exposure_mode=manual&exposure_us=10000&gain_mode=auto"
curl -X POST $B/af
curl -X POST "$B/io?light=1"
curl -o shot.jpg "$B/snapshot.jpg?q=95"
curl -X POST "$B/io?light=0"
```

```python
import requests
B = "http://192.168.11.164:8080"
requests.post(f"{B}/io", params={"light": 1}).raise_for_status()
jpg = requests.get(f"{B}/snapshot.jpg", params={"q": 95}, timeout=30).content
open("shot.jpg", "wb").write(jpg)
```
