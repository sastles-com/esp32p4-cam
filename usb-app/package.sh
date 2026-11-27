#!/bin/sh
# Windows などの PC に持っていく zip を作る: usb-app/dist/camera-usbapp-<日付>.zip
# 中身はリポジトリと同じ並び（usb-app/pc, usb-app/firmware のビルド済みバイナリ, common/webui）。
# server.py は ../../common/webui/index.html を読むので、この並びのまま展開して使う。
set -eu
cd "$(dirname "$0")"
NAME=camera-usbapp-$(date +%Y%m%d)
STAGE=dist/$NAME
B=firmware/build
[ -f $B/flash_args ] || { echo "$B/flash_args がありません。先にビルドしてください" >&2; exit 1; }
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE/usb-app/pc" "$STAGE/usb-app/firmware/build/bootloader" \
         "$STAGE/usb-app/firmware/build/partition_table" "$STAGE/common/webui"
cp pc/server.py pc/tunnel.py pc/httpch.py pc/requirements.txt pc/run.sh "$STAGE/usb-app/pc/"
cp firmware/flash.sh "$STAGE/usb-app/firmware/"
cp $B/flash_args $B/usbapp.bin "$STAGE/usb-app/firmware/build/"
cp $B/bootloader/bootloader.bin "$STAGE/usb-app/firmware/build/bootloader/"
cp $B/partition_table/partition-table.bin "$STAGE/usb-app/firmware/build/partition_table/"
cp README.md API.md "$STAGE/usb-app/"
cp ../common/webui/index.html "$STAGE/common/webui/"
cp WINDOWS.md "$STAGE/README.md"
# .bat は CRLF に
sed 's/$/\r/' pc/run.bat > "$STAGE/usb-app/pc/run.bat"
sed 's/$/\r/' firmware/flash.bat > "$STAGE/usb-app/firmware/flash.bat"
chmod +x "$STAGE/usb-app/pc/run.sh" "$STAGE/usb-app/firmware/flash.sh"
(cd dist && zip -qr "$NAME.zip" "$NAME")
rm -rf "$STAGE"
echo "dist/$NAME.zip"
