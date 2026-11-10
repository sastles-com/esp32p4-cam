#!/bin/sh
# .venv を用意して server.py を起動する。BOARD_HOST にボードの IP を入れておく。
set -eu
cd "$(dirname "$0")"
[ -d .venv ] || python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt
exec .venv/bin/python server.py "$@"
