# usb-app — USB でつないだボードを、PC の Web サーバーから操作する

ボードは USB だけで PC につなぐ（Wi-Fi は使わない）。PC 上の FastAPI サーバーが、wifi-app と同じ画面
（`common/webui/index.html`）を配信し、LAN の他の PC からも開ける。写真はブラウザでダウンロードする
（サーバーには保存しない）。アクセス制限はない（LAN 内で使う前提）。

```
他の PC / この PC のブラウザ ──HTTP :8080──▶ pc/server.py ──USB シリアル（トンネル）──▶ ボードの HTTP サーバー
```

## 使い方

1. ボードの **USB-C**（CH343）を PC につなぐ。
2. ファームを書き込む（初回とファーム更新時のみ）。PC サーバーは止めておく。
   ```sh
   pc/run.sh --help        # 初回: pc/.venv を作る（esptool もここに入る。ssl 付きの Python を自動で選ぶ）
   firmware/flash.sh       # ポートは自動。複数あるときは firmware/flash.sh /dev/cu.usbmodemXXXX
   ```
3. サーバーを起動する。
   ```sh
   pc/run.sh               # ポートは CH343 を自動で探す。--serial で指定もできる
   ```
   起動時に `http://192.168.x.x:8080/` のように表示される。その URL を、この PC または同じ LAN の PC の
   ブラウザで開く。macOS で「受信接続を許可しますか」と聞かれたら許可する。
4. コンソールには `[board]` 付きでボードのログが出る。`[tunnel] board: usb-app tunnel 2` が出れば接続済み。

wifi-app に戻すときは `wifi-app/firmware` を書き込む。保存した露出・ピント・WB の設定（`/cfg/config.json`）は
両アプリで同じ場所にあるので、そのまま引き継がれる。

HTTP API（`/ctrl`、`/snapshot.jpg`、`/io` など）の仕様は [API.md](API.md)。

## 速さ（2 Mbaud、実測 2026-11-12）

| 操作 | 結果 |
|---|---|
| 静止画 q90（約 230 KB） | 約 1.3 秒 |
| 静止画 q100（約 1.6 MB） | 約 8.2 秒 |
| プレビュー 960×536 q50 | 約 7 fps（閲覧者 2 つで同時に見ても同じ。ボードからの流れは 1 本） |
| 設定 API（`/ctrl` など） | 0.02〜0.35 秒 |

律速はシリアル（約 200 KB/s）。静止画の転送中はプレビューを止めて帯域を譲る。
計測はサーバーと同じ Mac から curl で行った（閲覧者 2 つ = curl 2 本）。別の PC からのアクセスは未確認。

## 注意

- **サーバーを起動するとボードがリセットされる**。macOS ではポートを開いた時点で DTR/RTS が動き、
  `server.py` が先に Low にしていても防げなかった。ボタンで撮ってボードに貯めた写真（4 枚のリング）は
  サーバーの起動で消える。AF も起動時にやり直す。
- サーバーはシリアルポートを開いたままにする。ファームを書き込むときは先にサーバーを止める。

## しくみ

- **ボード**（`firmware/`）: wifi-app と同じ HTTP サーバー（`common/camcore`、:80 と :81）を、Wi-Fi なしで動かす。
  コンソールの UART に **TCP のトンネル**（`main/tunnel.c`）を載せ、PC から開いたチャンネルごとに
  ボード自身の `127.0.0.1` へつなぐ。ログは同じ UART に普通に流れ、フレームの間に入る。
  フレーム形式は `tunnel.c` の先頭のコメント。
- **PC**（`pc/`）: `tunnel.py` がシリアルの相手側、`httpch.py` がチャンネル上の HTTP、`server.py` が
  画面の配信と中継。API は追加せず、ボードのパスをそのまま中継する。プレビューは上流 1 本を全閲覧者に配る。
- ファームと PC のトンネルの版は HELLO で照合する。合わないとサーバーが「flash usb-app/firmware」と表示し、
  チャンネルを開かない。

## ビルド（ファーム）

wifi-app と同じ環境（ESP-IDF 5.5.5、toolchain esp-14.2.0_20260121。HANDOFF.md の 2026-11-11 の節）で:

```sh
cd firmware && idf.py build
```

カメラ関係の sdkconfig は `common/camcore/sdkconfig.camcore`、この app のもの（lwIP のループバック用の
キュー、コンソール 2 Mbaud）は `sdkconfig.defaults`。チャンネルの開閉をログに出すには menuconfig の
「usb-app → Log every tunnel channel open and close」。サーバー側は `pc/run.sh --debug`。

## 未実装

- 経路 B: ネイティブ USB HS（MX1.25 4 ピン）の USB NCM。配線の確認が先。
- ボーレートの引き上げ（CH343 は 6 Mbaud まで。P4 側の上限と安定性を測ってから）。
