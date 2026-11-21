#!/bin/sh
# 先方に渡す zip を作る: mock-server/dist/camera-mock-<日付>.zip
# 中身は mock-server 一式 + Web 画面（common/webui）+ API 仕様書（usb-app/API.md）。
set -eu
cd "$(dirname "$0")"
NAME=camera-mock-$(date +%Y%m%d)
STAGE=dist/$NAME
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE/webui"
cp mock_server.py panel.html board_state.json requirements.txt run.sh run.bat README.md "$STAGE/"
cp ../common/webui/index.html "$STAGE/webui/"
cp ../usb-app/API.md "$STAGE/"
cp ../docs/mock-environment.md "$STAGE/"
# zip の中では同じフォルダにある
sed -i.bak 's#(../docs/mock-environment.md)#(mock-environment.md)#' "$STAGE/README.md" && rm "$STAGE/README.md.bak"
# run.bat は CRLF に
sed 's/$/\r/' run.bat > "$STAGE/run.bat"
chmod +x "$STAGE/run.sh"
(cd dist && zip -qr "$NAME.zip" "$NAME")
rm -rf "$STAGE"
echo "dist/$NAME.zip"
