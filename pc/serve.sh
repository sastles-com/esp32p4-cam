#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR/.."

exec pc/.venv/bin/python pc/camtool.py serve --board 172.20.10.6
