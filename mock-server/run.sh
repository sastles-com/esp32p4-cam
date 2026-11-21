#!/bin/sh
# .venv を用意して mock_server.py を起動する。引数はそのまま mock_server.py へ。
#   mock-server/run.sh [--port 8080] [--slow] [--no-camera] [--no-light-board] [--button-every 30] [--cors]
#   mock-server/run.sh --capture http://127.0.0.1:8080   （実機の中身を board_state.json に写して終わる）
# venv を作る Python は PYTHON で選べる。省くと ssl モジュールのあるものを探す（pip が使う）。
set -eu
cd "$(dirname "$0")"
if [ ! -d .venv ]; then
    for py in ${PYTHON:-} python3.13 python3.12 python3.11 python3 "$HOME/.platformio/penv/bin/python"; do
        if command -v "$py" >/dev/null 2>&1 && "$py" -c "import ssl" 2>/dev/null; then
            "$py" -m venv .venv
            break
        fi
    done
    [ -d .venv ] || { echo "ssl 付きの Python 3 が見つかりません。PYTHON=/path/to/python3 で指定してください" >&2; exit 1; }
fi
.venv/bin/pip install -q -r requirements.txt
exec .venv/bin/python mock_server.py "$@"
