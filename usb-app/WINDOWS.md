# カメラの PC サーバー（usb-app）を Windows で使う

ボード（ESP32-P4 + IMX708）を USB でつなぎ、この PC の Web サーバーから操作する。
画面はブラウザで開く。同じ LAN の他の PC からも開ける。

**フォルダの並びを変えないこと。** `usb-app\pc\server.py` は `common\webui\index.html` を読む。

## 用意するもの

- Python 3.10 以上（確認は 3.13）。https://www.python.org/downloads/ から入れる。
  インストーラの「Add python.exe to PATH」に印を付ける。
- ボードの USB-C（CH343）と PC をつなぐケーブル。
  デバイスマネージャーの「ポート (COM と LPT)」に COM ポートが出れば OK。
  出ないときは WCH の CH343 ドライバ（CH343SER）を入れる。

## 起動

`usb-app\pc\run.bat` をダブルクリックする（初回は依存パッケージを入れるので 1 分ほどかかる）。

- ポートは CH343（VID 1A86）を自動で探す。見つからないときはコマンドプロンプトで
  `run.bat --serial COM5` のように指定する。
- 起動すると `http://192.168.x.x:8080/` と `http://127.0.0.1:8080/` が表示される。ブラウザで開く。
- 「Windows セキュリティの重要な警告」（ファイアウォール）が出たら、プライベート ネットワークを許可する。
  許可しないと他の PC からは開けない（この PC からは開ける）。
- `[tunnel] board: usb-app tunnel 2` が出れば接続済み。
- 止めるときはウィンドウで Ctrl+C、または閉じる。
- **起動するとボードがリセットされる**ことがある。ボタンで撮ってボードに貯めた写真は消える。

レシピで撮った写真は `usb-app\pc\captures\<開始時刻>_<レシピ名>\` に保存される。
場所を変えるときは `run.bat --save-dir D:\photos`。

## ファームの書き込み（必要なときだけ）

ボードに usb-app のファームが入っていれば不要。サーバーが
「flash usb-app/firmware」と表示したときに書き込む。

1. サーバーを止める（ポートを開いたままだと書き込めない）。
2. `usb-app\pc\run.bat` を一度起動したことがあること（esptool がそこに入る）。
3. `usb-app\firmware\flash.bat` をダブルクリック（ポートは自動）。複数あるときは `flash.bat COM5`。

同梱のファームは v3.2 のボード用。

## 中身

| パス | 内容 |
|---|---|
| `usb-app\pc\` | サーバー本体（`server.py`, `tunnel.py`, `httpch.py`）、`run.bat`（Windows）、`run.sh`（macOS・Linux） |
| `usb-app\firmware\` | ビルド済みファーム（`build\`）と `flash.bat` / `flash.sh` |
| `common\webui\index.html` | 操作画面 |
| `usb-app\README.md` | 仕組みと速さ（macOS で書いたもの） |
| `usb-app\API.md` | HTTP API の仕様 |
