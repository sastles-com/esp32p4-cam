# カメラのダミーサーバー（mock-server）

カメラ装置（ESP32-P4 + IMX708）の PC サーバーと同じ HTTP API を返すダミーサーバー。
装置が無くてもフロントエンドを実装・確認できる。API の仕様は同梱の **[API.md](API.md)**
（リポジトリでは `usb-app/API.md`）。応答の形、ステータス、エラーの文言は実機に合わせてある。

**初めて使うときの手順は [docs/mock-environment.md](../docs/mock-environment.md)**（起動、確認できること、実機の中身の写し方、困ったとき）。

## 中身

| ファイル | 内容 |
|---|---|
| `mock_server.py` | ダミーサーバー本体（FastAPI） |
| `requirements.txt` | 依存パッケージ（版を固定） |
| `run.sh` / `run.bat` | 起動スクリプト（macOS・Linux / Windows）。初回に `.venv` を作って依存を入れる |
| `webui/index.html` | 今の操作画面。`/` で配信される。実装の参考に |
| `panel.html` | ダミー専用の操作パネル（`/_mock`） |
| `board_state.json` | 起動時の中身（プリセット・レシピ・カメラの設定・カメラと照明ボードの有無）。見本が入っている。`--capture` で実機の中身に置き換わる |
| `API.md` | API 仕様書 |

## 起動

Python 3.10 以上が必要（確認は 3.13）。

```sh
./run.sh                 # macOS / Linux
run.bat                  # Windows
```

手で行う場合:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt        # Windows: .venv\Scripts\pip ...
.venv/bin/python mock_server.py --port 8080
```

起動したら http://127.0.0.1:8080/ で今の画面、http://127.0.0.1:8080/_mock で操作パネルが開く。
オプションはそのまま渡せる（例: `./run.sh --slow --port 8090`）。

| オプション | 意味 |
|---|---|
| `--port 8080` / `--host 0.0.0.0` | 待ち受け（既定は全インターフェース、LAN の他の PC からも開ける） |
| `--slow` | シリアル相当の速さ（約 200 kB/s、JSON に 0.02〜0.35 秒の遅れ）。静止画の待ち表示などの確認に |
| `--no-camera` | カメラ無しのボード（`/ctrl` などが 404、`/shot` が 503、プレビューは届かない） |
| `--no-light-board` | 照明ボード無し（`POST /io` が 503、`/io` の `present` が false） |
| `--button-every 30` | 30 秒ごとに物理ボタンを押したことにする |
| `--fps 10` | プレビューの fps |
| `--cors` | `Access-Control-Allow-Origin: *` を付ける（実機のサーバーは付けない。下の注意を参照） |
| `--save-dir DIR` | レシピの結果を保存するフォルダ（既定: このフォルダの `captures/`）。usb-app の PC サーバーと同じ `/pc/info`・`/pc/save` |
| `--no-save` | PC に保存しない（画面はダウンロードに切り替わる。wifi-app と同じ動き） |
| `--state FILE` | 起動時の中身を読むファイル（既定: このフォルダの `board_state.json`。無ければ空のボード） |
| `--empty` | 中身を読まず、空のボードで始める |
| `--capture URL` | 実機の PC サーバー（例 `http://127.0.0.1:8080`）からプリセット・レシピ・設定を読み、`--state` のファイルに書いて終わる。実機は読むだけ |

## 実機と同じところ

- 全エンドポイント（`/ctrl`、`/ctrl/caps`、`/af`、`/wb/onepush`、`/config/reset`、`/snapshot.jpg`、
  `/snapshot.raw`、`/shot`、`/shot/info`、`/shot.jpg`、`/io`、`/presets`（保存・読み込み・削除・名前変更）、
  `/recipes`（保存・削除・名前変更。保存は JSON ボディ）、`/stats`、`/bench`、`/stream.mjpg`）。
- 知らないパスは 404 `Nothing matches the given URI`（テキスト）。
- `/af` と `/wb/onepush` は 202 で受け付けて裏で進む（AF は約 2.5 秒、レンズが動く様子も画に出る）。
  走っている間に呼ぶと 409。
- `/shot` と物理ボタンは、ボードの撮影設定（`shot_q`・`shot_light`）で撮る。照明を点けるときは約 0.4 秒待ってから撮り、最新 4 枚を貯める。
- プリセットは読み込むと全項目を適用する。向き（`hflip`・`vflip`）はプリセットに含まれないので、読み込みで「再起動」はしない。
  保存したプリセットとレシピはダミーの再起動では残るが、サーバーを止めると消え、次の起動で `board_state.json` の中身に戻る。
- レシピの保存は実機（`common/camcore/recipe.c`）と同じ検査をし、同じエラー文言を返す。省略したキーは既定値で埋めて記録する。`/shot/info` の `boot` は再起動で変わる。
- `hflip`・`vflip` を変える、または向きが付いた状態で `/config/reset` すると、0.8 秒後にボードが
  「再起動」し、約 3 秒間 503（`{"error":"board: board not connected"}`、PC サーバーと同じ文言）になる。貯めた写真は消える。
  PC サーバー自身の API（`/pc/info`・`/pc/save`）はその間も答える。
- 露出（シャッター優先のゲインを含む）、ゲイン、ピント（560 付近で合う）、WB、反転、照明が画に反映される。
  左上に今の設定と時刻が出る。

## 実機と違うところ

- 画像は合成画像（カラーバー、ジーメンススター、動く円）。
- `/stream.mjpg` は閲覧者ごとに独立（実機のサーバーは 1 本を全員に配り、`q`・`n` は最後の閲覧者のものになる）。
- 静止画の間にプレビューが止まる動きは無い。
- 設定は再起動では残るが、サーバーを止めると消える（次の起動で `board_state.json` の中身に戻る）。

## ダミー専用の操作

ブラウザで `http://127.0.0.1:8080/_mock` を開くと操作パネル（物理ボタン、再起動、カメラ・照明ボードの
有無、遅さの切り替え、USB を抜く）。同じことを HTTP でもできる:

```sh
curl -X POST http://127.0.0.1:8080/_mock/button                        # 物理ボタン → 202 {"id":N}
curl -X POST http://127.0.0.1:8080/_mock/reboot                        # 再起動（約 3 秒 503）
curl -X POST "http://127.0.0.1:8080/_mock/unplug?on=1"                 # USB を抜く（ボードの API が 503）。on=0 で挿す（起動し直す）
curl -X POST "http://127.0.0.1:8080/_mock/state?camera=0&light_board=1&slow=1"
curl http://127.0.0.1:8080/_mock/state
```

## 開発サーバー（Vite など）から使うとき

実機のサーバーは CORS ヘッダーを付けない（画面と API が同じオリジンの前提）。別ポートの開発サーバーから
呼ぶ場合は、開発サーバーのプロキシでこのサーバーへ転送するのがよい（本番と同じ形になる）。例: Vite

```js
// vite.config.js
export default { server: { proxy: {
  '^/(ctrl|af|wb|config|snapshot|shot|io|stats|bench|stream)': 'http://127.0.0.1:8080',
} } }
```

プロキシを使えない場合だけ `--cors` を付ける。
