#!/bin/sh
# .venv を用意して server.py を起動する。引数はそのまま server.py へ（--serial, --port, --debug）。
# venv を作る Python は PYTHON で選べる。省くと ssl モジュールのあるものを探す（pip が使う。
# この Mac の /usr/bin/python3 などは ssl が無く、pip install が失敗する）。
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
exec .venv/bin/python server.py "$@"
