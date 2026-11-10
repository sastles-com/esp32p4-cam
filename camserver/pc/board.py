"""ボード (imx708 の camctl ファーム) の HTTP クライアント。標準ライブラリのみ。"""
import json
import re
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

HOST_RE = re.compile(r"^[A-Za-z0-9.\-]{1,253}(:\d{1,5})?$")  # 実機はポート 80 固定。:port はテスト用
SHOT_TIMEOUT_S = 40  # 低速リンクでは 480 KB に数秒かかる
CTRL_TIMEOUT_S = 5
CTRL_KEYS = ("exposure_mode", "gain_mode", "exposure_us", "gain_index", "focus_mode", "focus_pos",
             "wb_mode", "wb_red", "wb_blue")

# 接続できない系の例外。呼び出し側はこれを 502 にする
NET_ERRORS = (urllib.error.URLError, TimeoutError, OSError)


class BoardError(Exception):
    """ボードがエラーを返した (status はそのまま中継する)。"""

    def __init__(self, status: int, detail: str):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def check_host(board: str) -> str:
    if not HOST_RE.match(board or ""):
        raise ValueError(f"ボードのアドレスが不正: {board!r}")
    return board


def call(board: str, method: str, path: str, params: dict | None = None):
    """ボードの制御 API を呼んで JSON を返す。"""
    url = f"http://{check_host(board)}{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=CTRL_TIMEOUT_S) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            detail = json.loads(body).get("error", body.decode(errors="replace"))
        except ValueError:
            detail = body.decode(errors="replace") or e.reason
        raise BoardError(e.code, detail)


def jpeg_size(data: bytes):
    """SOF マーカーから (幅, 高さ) を読む。読めなければ None。"""
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
    return None


def take_shot(board: str, quality: int, out_dir: Path) -> dict:
    """ボードからフルサイズ静止画を取得して保存する。"""
    check_host(board)
    quality = max(1, min(100, int(quality)))
    out_dir.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    with urllib.request.urlopen(f"http://{board}/snapshot.jpg?q={quality}", timeout=SHOT_TIMEOUT_S) as r:
        data = r.read()
    if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
        raise ValueError(f"JPEG として不完全 ({len(data)} bytes)")
    now = datetime.now()
    name = f"{now:%Y%m%d_%H%M%S}_{now.microsecond // 1000:03d}.jpg"  # 連写で上書きしない
    (out_dir / name).write_bytes(data)
    size = jpeg_size(data)
    return {"file": name, "bytes": len(data), "seconds": round(time.time() - t0, 2),
            "width": size[0] if size else None, "height": size[1] if size else None}
