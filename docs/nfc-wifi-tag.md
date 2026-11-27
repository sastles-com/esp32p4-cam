# ボードの AP に NFC タグ / QR でつなぐ

wifi-app のボードを自分の Wi-Fi（アクセスポイント）にして、スマホをタグや QR でつなぐ。
ルーターもテザリングも要らない。ボードは `http://192.168.4.1/`。

## 1. ボードを AP にする

`wifi-app/firmware/components/imx_wifi/include/wifi_credentials.h`（gitignore 済み）に書く:

```c
#define WIFI_AP_SSID      "imx-camera"
#define WIFI_AP_PASSWORD  "8〜63 文字"
#define WIFI_AP_CHANNEL   6
```

`WIFI_AP_SSID` があると AP、無ければ今まで通り `WIFI_STA_SSID` のネットワークに入る。
ビルドして書き込む（v3.2 のボード）:

```sh
source ~/.espressif/idf55/env.sh
cd wifi-app/firmware && idf build
cd build && ../../../usb-app/pc/.venv/bin/esptool -p <ポート> -b 460800 write-flash @flash_args
```

起動ログに `AP "imx-camera" up on channel 6 ... this board is 192.168.4.1` が出る。
スマホがつながるたびに `AP: client xx:xx:... joined` が出る。同時に 4 台まで。2.4 GHz のみ。

## 2. タグと QR を作る

```sh
pip install "qrcode[pil]"        # 初回だけ
python3 wifi-app/tools/nfc_tag.py
```

`wifi-app/nfc-out/`（gitignore 済み。パスワード入り）に次ができる:

| ファイル | 用途 |
|---|---|
| `wifi-qr.png` | スマホのカメラで読むと Wi-Fi に入る（iPhone・Android とも） |
| `url-qr.png` | 画面 `http://192.168.4.1/` を開く |
| `tag.ndef` | タグの中身（URL ＋ Wi-Fi の 2 レコード、109 バイト） |

### タグへの書き込み（スマホアプリ「NFC Tools」）

タグは NTAG213 以上（中身が 109 バイト、NTAG213 の容量は 137 バイト）。

1. 「書く」→「レコードを追加」→「URL / URI」: `http://192.168.4.1/`
2. 「レコードを追加」→「Wi-Fi ネットワーク」: 認証 WPA2-Personal、暗号化 AES、SSID とパスワード
   （`nfc_tag.py` が表示する値）
3. 「書く」でタグをかざす。必要なら「その他」→「ロック」で書き換えを禁止する（元に戻せない）。

順番は URL が先。iPhone は先頭の URL だけを読み、Android は Wi-Fi のレコードをどこにあっても拾う
（どちらも実機では未確認。反応しなければ順番を入れ替えて試す）。

## 3. 使い方

- **Android**: タグにかざす →「ネットワークに接続」→ ブラウザで `http://192.168.4.1/`
  （もう一度かざせば URL が開く）。
- **iPhone**: iPhone はタグの Wi-Fi 情報を使えない。初回は `wifi-qr.png` をカメラで読んで Wi-Fi に入る
  （一度入れば次からは自動）。その後タグにかざすと、通知から画面が開く。
- 「インターネットに接続されていません」と出たら、そのまま接続を続ける（ボードの AP はインターネットに出ない）。
  モバイルデータに切り替わって画面が開かないときは、Wi-Fi の設定でこのネットワークを「自動で切り替えない」にする。

## 注意

- タグや QR を読めば誰でもパスワードがわかる。置く場所に注意する。
- パスワードを変えたら、ビルドと書き込み、`nfc_tag.py` を出し直し、タグも書き直す。
- `camserver/` にも別の `imx_wifi` の写しがあるが、AP モードは `wifi-app` だけに入れた。
