#!/bin/sh
# ビルド済みの usb-app ファーム（build/）をボードに書き込む。
#   firmware/flash.sh [ポート]      ポートを省くと /dev/cu.usbmodem* が 1 つならそれを使う
# esptool は pc/.venv のもの（5.4.0。v3.2 のボードは 4.x では書き込めない）。
# PC サーバーが同じポートを開いていると書き込めないので、先に止めること。
set -eu
cd "$(dirname "$0")"
ESPTOOL=../pc/.venv/bin/esptool
[ -x "$ESPTOOL" ] || { echo "pc/.venv がありません。先に pc/run.sh を一度実行してください" >&2; exit 1; }
[ -f build/flash_args ] || { echo "build/flash_args がありません。先にビルドしてください" >&2; exit 1; }
PORT=${1:-}
if [ -z "$PORT" ]; then
    set -- /dev/cu.usbmodem*
    [ "$#" -eq 1 ] && [ -e "$1" ] || { echo "ポートを指定してください: $*" >&2; exit 1; }
    PORT=$1
fi
cd build
exec "../$ESPTOOL" -p "$PORT" -b 460800 write-flash "@flash_args"
