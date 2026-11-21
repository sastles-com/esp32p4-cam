# ダミー環境で動かす（ESP32-P4 が無くても動作確認する）

カメラ装置（ESP32-P4 + IMX708）が手元に無くても、PC 1 台で操作画面を開いて、撮影・プリセット・レシピ・
PC への保存までを確認できる。使うのは `mock-server/`（ダミーサーバー）で、実機の PC サーバー
（`usb-app/pc/server.py`）と同じ API・同じ画面を返す。

## 1. 用意するもの

- Python 3.10 以上（確認は 3.13）。macOS・Linux・Windows のどれでもよい。
- このリポジトリ一式。`mock-server/` だけでなく `common/webui/`（画面）も使う。

```sh
git clone https://github.com/sastles-com/esp32p4-cam.git
cd esp32p4-cam
git checkout claude/recipe-20261116      # main に入るまではこのブランチ
```

ESP-IDF、USB ケーブル、ボードはいらない。

**zip（`camera-mock-<日付>.zip`）で受け取った場合**は git もリポジトリもいらない。解凍したフォルダで
`./run.sh`（Windows は `run.bat` をダブルクリック）。以下の `mock-server/` は解凍したフォルダに読み替える
（例: `mock-server/run.sh --empty` → `./run.sh --empty`、写真は解凍したフォルダの `captures/` にできる）。
zip は `mock-server/package.sh` で作る（`mock-server/dist/` にできる）。

## 2. 起動する

```sh
mock-server/run.sh                # macOS / Linux
mock-server\run.bat               # Windows
```

初回は `mock-server/.venv` を作って必要なパッケージを入れる（1〜2 分）。次のように出たら起動済み:

```
中身: …/mock-server/board_state.json（sample …） プリセット 3 件、レシピ 2 件
usb-app mock: http://127.0.0.1:8080/   操作パネル: http://127.0.0.1:8080/_mock
```

止めるときは Ctrl+C。

| 開く URL | 中身 |
|---|---|
| http://127.0.0.1:8080/ | 操作画面（実機と同じ画面） |
| http://127.0.0.1:8080/_mock | ダミー専用の操作パネル（物理ボタン、再起動、USB を抜く、カメラ・照明ボードの有無） |

LAN の他の PC からは `http://<この PC の IP>:8080/` で開ける。ポートを変えるときは `mock-server/run.sh --port 8090`。

## 3. 起動時の中身

ダミーは `mock-server/board_state.json` を読んで、実機に似た状態で始まる。

- プリセット: 「標準」（全部オート）、「試験片A」（全部マニュアル）、「シャッター優先 4ms」
- レシピ: 「露出ブラケット5段」（標準を元に露出を 2〜32 ms）、「照明あり・なし」
- カメラあり、照明ボードあり

画面で足したり変えたりしたプリセット・レシピは、ダミーを止めると消えて、次の起動でまた上の中身に戻る。
空のボードで始めたいときは `mock-server/run.sh --empty`。

### 実機の中身をダミーに写す

実機の PC サーバーが動いているとき（ボードを USB でつないで `usb-app/pc/run.sh`）に、次を実行する:

```sh
mock-server/run.sh --capture http://127.0.0.1:8080
```

実機のプリセット・レシピ・カメラの設定・カメラと照明ボードの有無を読んで `board_state.json` に書き、
すぐ終わる（実機は読むだけで何も変えない）。以後のダミーはその中身で始まる。ダミーと実機の PC サーバーは
同じポート 8080 を使うので、写したあとでダミーを起動するときは実機のサーバーを止めておくか `--port` を変える。

見本に戻すには `git checkout mock-server/board_state.json`。

## 4. 確認できること

| 確認すること | やり方 |
|---|---|
| プレビュー、露出・ゲイン・ピント・WB | 画面の「カメラ」。合成画像に設定が反映される（左上に今の値） |
| 撮影、照明 | 画面の撮影ボタン、または操作パネルの「物理ボタンを押す」 |
| プリセットの保存・読み込み | 画面の「プリセット」 |
| レシピの作成・実行 | 画面の「レシピ」。行の「P」でプリセットの値を入れられる |
| PC への保存 | レシピを実行すると `mock-server/captures/<開始時刻>_<レシピ名>/` に写真・レシピ JSON・撮影条件ができる |
| ダウンロードに切り替わる動き（wifi-app 相当） | `mock-server/run.sh --no-save` |
| シリアルの遅さ | `mock-server/run.sh --slow`（約 200 kB/s）。パネルでも切り替えられる |
| USB が抜けたとき | パネルの「USB を抜く」。ボードの API が `503 board: board not connected` になる。戻すとボードは起動し直す |
| カメラ無し・照明ボード無しのボード | パネルのチェック、または `--no-camera`・`--no-light-board` |

## 5. 実機と違うところ

- 画像は合成画像（カラーバー、ジーメンススター、動く円）。ピントはレンズ位置 560 付近で合う。
- プレビューは閲覧者ごとに独立して届く（実機は 1 本を全員で分ける）。静止画の間もプレビューは止まらない。
- 実機の書き込み（ファームウェアの更新）や USB の不調は真似できない。

API の細かい違いと、ダミー専用の操作の一覧は [mock-server/README.md](../mock-server/README.md)。
API の仕様は [usb-app/API.md](../usb-app/API.md)。

## 6. うまくいかないとき

| 症状 | 対処 |
|---|---|
| `ssl 付きの Python 3 が見つかりません` | `PYTHON=/path/to/python3 mock-server/run.sh` で Python を指定する |
| `address already in use` | 8080 を別のもの（実機の PC サーバーなど）が使っている。止めるか `--port 8090` |
| 画面が古いまま | ブラウザで再読み込み。画面は `common/webui/index.html` を毎回読む |
| `Web 画面が見つかりません` | `mock-server/` だけを取り出している。リポジトリ一式（`common/webui/` を含む）で動かす |
| `--capture` で `に届きません` | 実機の PC サーバーが動いていない、または URL・ポートが違う |
