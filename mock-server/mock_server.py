"""usb-app のダミーサーバー: ボードもカメラも無しで、usb-app/pc/server.py と同じ API を返す。

    mock-server/run.sh [--port 8080] [--slow] [--no-camera] [--no-light-board] [--cors] [--button-every 30]

フロントエンドの開発用。応答の形・ステータス・エラー文言は usb-app/API.md と実機
（common/camcore）に合わせてある。画像は Pillow で描いた合成画像で、露出・ゲイン・ピント・WB・
反転・照明の設定が見た目に反映される（左上に今の設定が出る）。

実機に無い、ダミー専用の操作（/_mock/…）:
  GET  /_mock                  操作パネル（ブラウザで開く）
  POST /_mock/button           物理ボタンを押したことにする
  POST /_mock/reboot           ボードの再起動（約 3 秒 503、貯めた写真は消える）
  POST /_mock/unplug?on=0|1    USB を抜いた / 挿した（抜いている間、ボードの API は 503）
  GET  /_mock/state            ダミーの設定
  POST /_mock/state?camera=0|1&light_board=0|1&slow=0|1   その場で切り替える

起動時の中身（プリセット・レシピ・カメラの設定）は board_state.json から読む（無ければ空）。
実機の今の中身を写すには、実機の PC サーバーを動かした状態で:
    mock-server/run.sh --capture http://127.0.0.1:8080      # board_state.json を書いて終わる
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import io
import json
import math
import random
import re
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, Response, StreamingResponse
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
# 配布用 zip では同じフォルダの webui/、リポジトリでは common/webui/ の画面を使う
WEBUI = next((p for p in (HERE / "webui" / "index.html", HERE.parent / "common" / "webui" / "index.html")
              if p.exists()), HERE / "webui" / "index.html")

# ---- 実機の値（IMX708 1920x1080 モード、common/camcore） -------------------------
FULL = (1920, 1080)
SIZES = {16: (1920, 1080), 12: (1440, 792), 8: (960, 536), 4: (480, 264)}
TLINE_NS = 13361
LINES_MIN, LINES_MAX, LINES_STEP = 4, 2624, 2
GAIN_X = [1.123, 1.189, 1.261, 1.335, 1.414, 1.499, 1.588, 1.681, 1.781, 1.889, 2.000, 2.120,
          2.246, 2.381, 2.522, 2.674, 2.829, 2.994, 3.180, 3.368, 3.568, 3.779, 4.000, 4.231,
          4.491, 4.763, 5.044, 5.333, 5.657, 5.988, 6.360, 6.737, 7.111, 7.529, 8.000, 8.463,
          8.982, 9.481, 10.039, 10.667, 11.378, 12.047, 12.642, 13.474, 14.222, 15.059, 16.000]
FOCUS_MIN, FOCUS_MAX, AF_SCAN_MIN, AF_SCAN_MAX = 0, 1023, 448, 960
WB_MIN, WB_MAX = 300, 3999
LIGHT_MASK = 0x03
CTRL_KEYS = {"exposure_mode", "gain_mode", "exposure_us", "gain_index", "focus_mode", "focus_pos",
             "wb_mode", "wb_red", "wb_blue", "sp", "hflip", "vflip", "shot_q", "shot_light"}
PRESET_NAME_MAX, PRESETS_MAX = 48, 20      # 名前は UTF-8 で 48 バイトまで、20 個まで
BAD_NAME = 'name must be 1-48 bytes of text, without " or \\'
RING = 4                 # /shot で貯める枚数
SERIAL_BPS = 200_000     # --slow のときの転送速度（2 Mbaud のシリアル相当）
REBOOT_S = 3.0

# 合成シーンの「正しい」値: ここから離れるほど暗く / 明るく / ぼける / 色がずれる
AUTO_EXP_US, AUTO_GAIN = 33322, 9          # 照明なしで AE が落ち着く値（実機の観測値）
SHARP_POS = 560                            # ピントが合うレンズ位置
NEUTRAL_RB = (1820, 1918)                  # この合成画像で白が白になる WB
AUTO_RB = (1704, 2002)                     # グレーワールドの結果（少しずれる）
LIGHT_GAIN = 4.0                           # 照明を点けたときのシーンの明るさ

NOT_FOUND = "Nothing matches the given URI"
NOT_CONNECTED = "board: board not connected"   # usb-app/pc/server.py が返す 503 の文言（tunnel.py）
STATE_FILE = HERE / "board_state.json"


def lines_to_us(lines: int) -> int:
    return round(lines * TLINE_NS / 1000)


def us_to_lines(us: int) -> int:
    lines = round(us * 1000 / TLINE_NS)
    lines -= lines % LINES_STEP
    return max(LINES_MIN, min(LINES_MAX, lines))


def nearest_gain(x: float) -> int:
    return min(range(len(GAIN_X)), key=lambda i: abs(GAIN_X[i] - x))


class Board:
    """ボードの状態。実機の camctl / shot / ioexp の振る舞いを真似る。"""

    def __init__(self, camera: bool, light_board: bool):
        self.camera = camera
        self.light_board = light_board
        # 再起動しても残るもの（実機では config.json）
        self.cfg = self._default_cfg()
        self.config = "default"
        self.presets: list[dict] = []          # 実機の presets.json（再起動でも残る）
        self.recipes: list[dict] = []          # 実機の recipes.json（再起動でも残る）
        self.preset: str | None = None         # 最後に読み込んだ / 保存したプリセット
        self.preset_modified = False
        self.unplugged = False                 # USB が抜けている（PC サーバーはボードに届かない）
        self.boot_up(af=False)        # 起動時の AF はイベントループが動いてから（lifespan）

    @staticmethod
    def _default_cfg() -> dict:
        return dict(exposure_mode="auto", gain_mode="auto", focus_mode="auto", wb_mode="auto",
                    exp_lines=us_to_lines(AUTO_EXP_US), gain_index=AUTO_GAIN, focus_pos=512,
                    wb_red=AUTO_RB[0], wb_blue=AUTO_RB[1], sp="B", hflip=False, vflip=False,
                    shot_q=95, shot_light=True)

    def boot_up(self, af: bool = True) -> None:
        """起動直後の状態（再起動で消えるもの）。"""
        self.boot = "%08x" % random.getrandbits(32)
        self.down_until = 0.0
        self.focus = self.cfg["focus_pos"]
        self.af = dict(state="idle", score=0, ms=0)
        self.wb_state = "idle"
        self.out = 0
        self.shots: list[dict] = []
        self.next_id, self.done_id = 1, 0
        self.queue: list[tuple[int, int, bool, str]] = []
        self.busy = False
        self.frames = 0
        self.stats = dict(last="none", bytes=0, encode_ms=0, send_ms=0, mbit_s=0.0)
        if af and self.camera and self.cfg["focus_mode"] == "auto":
            asyncio.create_task(self.run_af())                 # 実機も起動時に AF を 1 回

    @property
    def down(self) -> bool:
        return self.unplugged or time.monotonic() < self.down_until

    # ---- 起動時の中身（board_state.json）--------------------------------------

    def load_state(self, d: dict) -> None:
        """--capture で実機から写した中身を入れる。"""
        self.camera = d.get("camera", self.camera)
        self.light_board = d.get("light_board", self.light_board)
        self.presets = [dict(p) for p in d.get("presets", [])]
        self.recipes = [dict(r) for r in d.get("recipes", [])]
        c = d.get("ctrl")
        if c:
            cfg = self.cfg
            for k in ("exposure_mode", "gain_mode", "focus_mode", "wb_mode", "gain_index", "focus_pos",
                      "wb_red", "wb_blue", "hflip", "vflip", "shot_q", "shot_light"):
                if k in c:
                    cfg[k] = c[k]
            if "exposure_us" in c:
                cfg["exp_lines"] = us_to_lines(c["exposure_us"])
            if c.get("shutter_priority") in ("A", "B"):
                cfg["sp"] = c["shutter_priority"]
            self.config = c.get("config", "loaded")
            self.preset = c.get("preset")
            self.preset_modified = bool(c.get("preset_modified"))
            self.focus = cfg["focus_pos"]

    def reboot(self, delay: float = 0.8) -> None:
        async def go():
            await asyncio.sleep(delay)
            self.down_until = time.monotonic() + REBOOT_S
            await asyncio.sleep(REBOOT_S)
            self.boot_up()
        asyncio.create_task(go())

    # ---- 露出の見え方 ----------------------------------------------------------

    @property
    def light_on(self) -> bool:
        return self.light_board and (self.out & LIGHT_MASK) == LIGHT_MASK

    def exposure(self) -> tuple[int, int, float]:
        """(露出の行数, ゲイン番号, 画の明るさ 1.0 = 適正)。"""
        scene = LIGHT_GAIN if self.light_on else 1.0
        target = lines_to_us(us_to_lines(AUTO_EXP_US)) * GAIN_X[AUTO_GAIN] / scene   # 適正になる us×倍率
        c = self.cfg
        if c["exposure_mode"] == "auto":
            lines = us_to_lines(min(AUTO_EXP_US, target / GAIN_X[0]))
            gi = nearest_gain(target / lines_to_us(lines))
        else:
            lines = c["exp_lines"]
            gi = c["gain_index"] if c["gain_mode"] == "manual" else nearest_gain(target / lines_to_us(lines))
        return lines, gi, lines_to_us(lines) * GAIN_X[gi] / target

    def wb_gains(self) -> tuple[int, int]:
        if self.cfg["wb_mode"] == "auto":
            return AUTO_RB
        return self.cfg["wb_red"], self.cfg["wb_blue"]

    def state(self) -> dict:
        lines, gi, _ = self.exposure()
        r, b = self.wb_gains()
        c = self.cfg
        return {
            "exposure_mode": c["exposure_mode"], "gain_mode": c["gain_mode"], "focus_mode": c["focus_mode"],
            "exposure_us": lines_to_us(lines), "exposure_lines": lines,
            "gain_index": gi, "gain_x": round(GAIN_X[gi], 2),
            "focus_pos": self.focus, "af_state": self.af["state"], "af_score": self.af["score"],
            "af_ms": self.af["ms"],
            "wb_mode": c["wb_mode"], "wb_red": r, "wb_blue": b, "wb_state": self.wb_state,
            "hflip": c["hflip"], "vflip": c["vflip"],
            "shutter_priority": c["sp"], "shot_q": c["shot_q"], "shot_light": c["shot_light"],
            "preset": self.preset, "preset_modified": bool(self.preset and self.preset_modified),
            "config": self.config,
        }

    def apply(self, v: dict, force_af: bool = False) -> bool:
        """POST /ctrl とプリセットの読み込みが共通で使う。向きが変わったら True（呼び手が再起動する）。"""
        c = self.cfg
        lines, gi, _ = self.exposure()
        if "exposure_mode" in v and v["exposure_mode"] != c["exposure_mode"]:
            if v["exposure_mode"] == "manual":       # 今 AE が使っている値から始める
                if "exposure_us" not in v:
                    c["exp_lines"] = lines
                if "gain_index" not in v:
                    c["gain_index"] = gi
            c["exposure_mode"] = v["exposure_mode"]
        if "gain_mode" in v and v["gain_mode"] != c["gain_mode"]:
            if v["gain_mode"] == "manual" and "gain_index" not in v:
                c["gain_index"] = gi
            c["gain_mode"] = v["gain_mode"]
        if "exposure_us" in v:
            c["exp_lines"] = us_to_lines(max(0, v["exposure_us"]))
        if "gain_index" in v:
            c["gain_index"] = max(0, min(len(GAIN_X) - 1, v["gain_index"]))
        if "sp" in v:
            c["sp"] = v["sp"]
        start_af = False
        if "focus_pos" in v:
            c["focus_pos"] = max(FOCUS_MIN, min(FOCUS_MAX, v["focus_pos"]))
        if "focus_mode" in v:
            start_af = v["focus_mode"] == "auto" and c["focus_mode"] == "manual"
            c["focus_mode"] = v["focus_mode"]
        if force_af and c["focus_mode"] == "auto":
            start_af = True
        if c["focus_mode"] == "manual" and ("focus_pos" in v or "focus_mode" in v):
            self.focus = c["focus_pos"]
        if "wb_mode" in v:
            c["wb_mode"] = v["wb_mode"]
        for k in ("wb_red", "wb_blue"):
            if k in v:
                c[k] = max(WB_MIN, min(WB_MAX, v[k]))
        if "shot_q" in v:
            c["shot_q"] = max(1, min(100, v["shot_q"]))
        if "shot_light" in v:
            c["shot_light"] = bool(v["shot_light"])
        flip = any(k in v and v[k] != c[k] for k in ("hflip", "vflip"))
        for k in ("hflip", "vflip"):
            if k in v:
                c[k] = v[k]
        if start_af and self.af["state"] != "running":
            asyncio.create_task(self.run_af())
        if v:
            self.saved()
        return flip

    def caps(self) -> dict:
        return {
            "tline_ns": TLINE_NS,
            "exposure_us": {"min": lines_to_us(LINES_MIN), "max": lines_to_us(LINES_MAX),
                            "step": lines_to_us(LINES_STEP)},
            "gain": {"min": 0, "max": len(GAIN_X) - 1, "x": GAIN_X},
            "focus": {"min": FOCUS_MIN, "max": FOCUS_MAX, "scan_min": AF_SCAN_MIN, "scan_max": AF_SCAN_MAX},
            "wb": {"min": WB_MIN, "max": WB_MAX},
            "shutter_priority": self.cfg["sp"],
        }

    def io(self) -> dict:
        return {"present": self.light_board, "addr": 32, "out": self.out if self.light_board else 0,
                "light": self.light_on}

    def saved(self) -> None:
        self.config = "loaded"

    # ---- AF / WB / 撮影 -----------------------------------------------------

    async def run_af(self) -> None:
        boot = self.boot
        self.af["state"] = "running"
        self.cfg["focus_mode"] = "auto"
        t0 = time.monotonic()
        for pos in range(AF_SCAN_MIN, AF_SCAN_MAX + 1, 32):     # レンズが動く様子も画に出る
            if self.boot != boot:
                return
            self.focus = pos
            await asyncio.sleep(0.12)
        best = SHARP_POS + random.randint(-8, 8)
        self.focus = best
        await asyncio.sleep(0.4)
        if self.boot != boot:
            return
        self.af = dict(state="done", score=380 + random.randint(-20, 20),
                       ms=round((time.monotonic() - t0) * 1000))
        self.cfg["focus_pos"] = best

    async def run_onepush(self) -> None:
        boot = self.boot
        self.wb_state = "running"
        await asyncio.sleep(1.0)
        if self.boot != boot:
            return
        self.cfg.update(wb_mode="manual", wb_red=NEUTRAL_RB[0] + random.randint(-15, 15),
                        wb_blue=NEUTRAL_RB[1] + random.randint(-15, 15))
        self.wb_state = "done"
        self.saved()

    def request_shot(self, q: int, light: bool, src: str) -> int | None:
        if len(self.queue) >= 2:
            return None
        sid = self.next_id
        self.next_id += 1
        self.queue.append((sid, q, light, src))
        if not self.busy:
            asyncio.create_task(self._shooter())
        return sid

    async def _shooter(self) -> None:
        boot = self.boot
        self.busy = True
        while self.queue and self.boot == boot:
            sid, q, light, src = self.queue.pop(0)
            lit = light and self.light_board
            was = self.out
            already = (was & LIGHT_MASK) == LIGHT_MASK   # 点けたままなら待たずに撮る（実機と同じ）
            settle = 0
            if lit and not already:
                self.out |= LIGHT_MASK
                settle = 380 + random.randint(0, 80)          # 露出が落ち着くまで
                await asyncio.sleep(settle / 1000)
            if self.camera:
                jpeg = await asyncio.to_thread(render_jpeg, self, FULL, q, f"#{sid} {src}")
                self.shots.append(dict(id=sid, bytes=len(jpeg), light=lit, settle_ms=settle,
                                       t=time.monotonic(), src=src, jpeg=jpeg))
                del self.shots[:-RING]
            else:
                await asyncio.sleep(0.4)                      # カメラ無し: 照明が光るだけ
            if lit and not already:
                self.out = was                                # 点いていなかったなら消す
            lit = lit or (already and self.light_board)
            self.done_id = sid
        self.busy = False

    def shot_info(self) -> dict:
        now = time.monotonic()
        return {"boot": self.boot, "busy": self.busy, "next": self.next_id, "done": self.done_id,
                "shots": [{k: s[k] for k in ("id", "bytes", "light", "settle_ms", "src")}
                          | {"age_ms": round((now - s["t"]) * 1000)} for s in self.shots]}


# ---- 合成画像 ---------------------------------------------------------------

_base_cache: dict[tuple[int, int], Image.Image] = {}


def _font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:                     # Pillow < 10.1
        return ImageFont.load_default()


def _base(size: tuple[int, int]) -> Image.Image:
    """動かない背景（グラデーション、カラーバー、ピント用の放射状チャート）。"""
    if size in _base_cache:
        return _base_cache[size]
    w, h = size
    img = Image.linear_gradient("L").rotate(90).resize((w, h))
    img = Image.merge("RGB", [img.point(lambda v: 40 + v // 3), img.point(lambda v: 60 + v // 4),
                              img.point(lambda v: 90 + v // 5)])
    d = ImageDraw.Draw(img)
    bars = [(235, 235, 235), (235, 235, 16), (16, 235, 235), (16, 235, 16), (235, 16, 235),
            (235, 16, 16), (16, 16, 235), (128, 128, 128)]
    bw = w // 2 // len(bars)
    for i, c in enumerate(bars):
        d.rectangle([w // 2 + i * bw, int(h * .62), w // 2 + (i + 1) * bw, int(h * .92)], fill=c)
    cx, cy, r = int(w * .25), int(h * .62), int(h * .28)          # ジーメンススター
    for k in range(36):
        if k % 2:
            a0, a1 = k * 10, (k + 1) * 10
            d.pieslice([cx - r, cy - r, cx + r, cy + r], a0, a1, fill=(20, 20, 20))
        else:
            d.pieslice([cx - r, cy - r, cx + r, cy + r], k * 10, (k + 1) * 10, fill=(225, 225, 225))
    gx, gy, gw = int(w * .5), int(h * .08), int(w * .46)          # 白い板（WB の基準）
    d.rectangle([gx, gy, gx + gw, gy + int(h * .45)], fill=(200, 200, 200))
    _base_cache[size] = img
    return img


def render(board: Board, size: tuple[int, int], label: str = "") -> Image.Image:
    w, h = size
    img = _base(size).copy()
    d = ImageDraw.Draw(img)
    t = time.time()
    # 動くもの（フレームが更新されているのが分かるように）
    x = int((w * .55) + (w * .38) * (0.5 + 0.5 * math.sin(t * 0.8)))
    r = max(6, h // 14)
    d.ellipse([x - r, int(h * .3) - r, x + r, int(h * .3) + r], fill=(240, 120, 20))
    st = board.state()
    fs = max(10, h // 30)
    lines = [
        f"MOCK  {time.strftime('%H:%M:%S')}.{int(t * 10) % 10}  frame {board.frames}  {label}",
        f"exp {st['exposure_mode']} {st['exposure_us']} us  gain {st['gain_mode']} x{st['gain_x']}",
        f"focus {st['focus_mode']} {st['focus_pos']} ({st['af_state']})  wb {st['wb_mode']} R{st['wb_red']} B{st['wb_blue']}",
        f"light {'ON' if board.light_on else 'off'}  flip h={int(st['hflip'])} v={int(st['vflip'])}",
    ]
    font = _font(fs)
    d.rectangle([0, 0, int(w * .47), int(fs * 1.3 * len(lines) + fs * .6)], fill=(0, 0, 0))
    for i, s in enumerate(lines):
        d.text((fs // 2, fs // 3 + i * int(fs * 1.3)), s, fill=(255, 255, 255), font=font)
    d.text((int(w * .52), int(h * .14)), "R  (orientation)", fill=(30, 30, 30), font=_font(fs * 3))

    # ピント: SHARP_POS から離れるほどぼかす（文字以外も含めて、レンズと同じく画全体）
    blur = abs(board.focus - SHARP_POS) / 40 * (w / 1920)
    if blur > 0.3:
        img = img.filter(ImageFilter.GaussianBlur(blur))
    # 露出と WB: チャンネルごとに掛ける（白飛びは 255 で切れる）
    _, _, bright = board.exposure()
    rg, bg = board.wb_gains()
    kr = bright * rg / NEUTRAL_RB[0] * (1.08 if board.light_on else 1.0)   # 照明は少し暖色
    kg = bright
    kb = bright * bg / NEUTRAL_RB[1] * (0.92 if board.light_on else 1.0)
    img = Image.merge("RGB", [ch.point(lambda v, k=k: min(255, int(v * k)))
                              for ch, k in zip(img.split(), (kr, kg, kb))])
    if st["hflip"]:
        img = img.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    if st["vflip"]:
        img = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    return img


def render_jpeg(board: Board, size: tuple[int, int], q: int, label: str = "") -> bytes:
    t0 = time.perf_counter()
    buf = io.BytesIO()
    render(board, size, label).save(buf, "JPEG", quality=max(1, min(q, 100)), subsampling=1)   # 4:2:2
    data = buf.getvalue()
    board.stats.update(bytes=len(data), encode_ms=round((time.perf_counter() - t0) * 1000))
    return data


def render_rgb565(board: Board) -> bytes:
    """RGB565、1 画素 2 バイトのリトルエンディアン（上位 5 ビットが R）。"""
    rgb = render(board, FULL).tobytes()
    out = bytearray(len(rgb) // 3 * 2)
    for i in range(0, len(rgb) // 3):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = v >> 8
    return bytes(out)


# ---- アプリ ----
# 注意: asyncio.create_task を呼ぶエンドポイントは async def にすること（def はスレッドで動き、ループが無い）------------------------------------------------------------

def build_app(board: Board, opts: argparse.Namespace) -> FastAPI:
    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        board.boot_up()
        tasks = []
        if opts.button_every:
            async def presser():
                while True:
                    await asyncio.sleep(opts.button_every)
                    if not board.down:
                        board.request_shot(board.cfg["shot_q"], board.cfg["shot_light"], "button")
            tasks.append(asyncio.create_task(presser()))
        yield
        for t in tasks:
            t.cancel()

    app = FastAPI(title="usb-app mock", lifespan=lifespan, docs_url=None, redoc_url=None)

    def err(status: int, msg: str) -> JSONResponse:
        return JSONResponse({"error": msg}, status_code=status)

    def not_found() -> PlainTextResponse:
        return PlainTextResponse(NOT_FOUND, status_code=404)

    async def latency() -> None:
        if opts.slow:
            await asyncio.sleep(random.uniform(0.02, 0.35))

    async def send(data: bytes, media_type: str, headers: dict | None = None) -> Response:
        """--slow なら、シリアルの速さで少しずつ送る。"""
        headers = {"Cache-Control": "no-store", **(headers or {})}
        if not opts.slow:
            return Response(data, media_type=media_type, headers=headers)

        async def trickle():
            step = 8192
            for i in range(0, len(data), step):
                yield data[i:i + step]
                await asyncio.sleep(step / SERIAL_BPS)
        headers["Content-Length"] = str(len(data))
        return StreamingResponse(trickle(), media_type=media_type, headers=headers)

    @app.middleware("http")
    async def board_gate(req: Request, call_next):
        p = req.url.path
        if p != "/" and not p.startswith(("/_mock", "/pc/")) and p != "/stream.mjpg":
            await latency()
            if board.down:
                return err(503, NOT_CONNECTED)        # 再起動中・USB が抜けている（server.py と同じ）
        rep = await call_next(req)
        if opts.cors:
            rep.headers["Access-Control-Allow-Origin"] = "*"
            rep.headers["Access-Control-Expose-Headers"] = "X-Shot-Id, X-Frame-Size, X-Frame-Format"
        return rep

    # ---- サーバー固有（server.py と同じ） ----------------------------------

    @app.get("/", response_class=HTMLResponse)
    def index():
        html = WEBUI.read_text(encoding="utf-8")
        inject = '<script>window.STREAM_URL="/stream.mjpg"</script>\n<script>'
        return HTMLResponse(html.replace("<script>", inject, 1), headers={"Cache-Control": "no-store"})

    @app.get("/stream.mjpg")
    async def stream(q: int = 50, n: int = 8):
        q = max(1, min(q, 100))

        async def frames():
            if not board.camera or n not in SIZES:
                # 実機: カメラ無しなら :81 が無い、n が違えば上流が 400。どちらもフレームは届かない。
                while True:
                    await asyncio.sleep(3600)
            fps = opts.fps if not opts.slow else min(opts.fps, 7 * 8 / n)
            while True:
                t0 = time.monotonic()
                if not board.down:
                    jpeg = await asyncio.to_thread(render_jpeg, board, SIZES[n], q)
                    board.frames += 1
                    board.stats["last"] = "mjpeg"
                    yield (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg)
                           + jpeg + b"\r\n")
                await asyncio.sleep(max(0.0, 1 / fps - (time.monotonic() - t0)))

        return StreamingResponse(frames(), media_type="multipart/x-mixed-replace; boundary=frame",
                                 headers={"Cache-Control": "no-store"})

    # ---- カメラの状態と設定 --------------------------------------------------

    @app.get("/ctrl")
    def ctrl_get():
        return board.state() if board.camera else not_found()

    @app.get("/ctrl/caps")
    def ctrl_caps():
        return board.caps() if board.camera else not_found()

    @app.post("/ctrl")
    async def ctrl_post(req: Request):
        if not board.camera:
            return not_found()
        qp = req.query_params
        if any(k not in CTRL_KEYS for k in qp.keys()):
            return err(400, "unknown key")
        v: dict = {}
        for k in ("exposure_mode", "gain_mode", "focus_mode", "wb_mode"):
            if k in qp:
                if qp[k] not in ("auto", "manual"):
                    return err(400, f"{k} must be auto or manual")
                v[k] = qp[k]
        for k in ("exposure_us", "gain_index", "focus_pos", "wb_red", "wb_blue", "shot_q"):
            if k in qp:
                try:
                    v[k] = int(qp[k])
                except ValueError:
                    return err(400, f"{k} must be an integer")
        if "sp" in qp:
            if qp["sp"] not in ("A", "B"):
                return err(400, "sp must be A or B")
            v["sp"] = qp["sp"]
        for k in ("hflip", "vflip", "shot_light"):
            if k in qp:
                if qp[k] not in ("0", "1"):
                    return err(400, f"{k} must be 0 or 1")
                v[k] = qp[k] == "1"
        flip = board.apply(v)
        if board.preset and any(k not in ("hflip", "vflip") for k in v):
            board.preset_modified = True             # 向きはプリセットの項目ではない
        state = board.state()
        if flip:
            board.reboot()                           # 実機: 保存して応答し、0.8 秒後に再起動
        return state

    @app.post("/af")
    async def af():
        if not board.camera:
            return not_found()
        if board.af["state"] == "running":
            return err(409, "af already running")
        if board.cfg["focus_mode"] != "auto" and board.preset:
            board.preset_modified = True
        board.af["state"] = "running"
        asyncio.create_task(board.run_af())
        return JSONResponse({"af_state": "running"}, status_code=202)

    @app.post("/wb/onepush")
    async def onepush():
        if not board.camera:
            return not_found()
        if board.wb_state == "running":
            return err(409, "one-push already running")
        if board.preset:
            board.preset_modified = True                   # 終わると WB は manual になる
        asyncio.create_task(board.run_onepush())
        return JSONResponse({"wb_state": "running"}, status_code=202)

    @app.post("/config/reset")
    async def config_reset():
        if not board.camera:
            return not_found()
        flipped = board.cfg["hflip"] or board.cfg["vflip"]
        board.cfg = board._default_cfg()
        board.config = "default"
        board.preset, board.preset_modified = None, False   # プリセット自体は消えない
        board.focus = board.cfg["focus_pos"]
        state = board.state()
        if flipped:
            board.reboot()
        return state

    # ---- 静止画 -------------------------------------------------------------

    @app.get("/snapshot.jpg")
    async def snapshot(q: int = 90):
        if not board.camera:
            return not_found()
        jpeg = await asyncio.to_thread(render_jpeg, board, FULL, q, "snapshot")
        board.stats["last"] = "jpeg"
        return await send(jpeg, "image/jpeg")

    @app.get("/snapshot.raw")
    async def snapshot_raw():
        if not board.camera:
            return not_found()
        raw = await asyncio.to_thread(render_rgb565, board)
        board.stats.update(last="raw", bytes=len(raw))
        return await send(raw, "application/octet-stream",
                          {"X-Frame-Size": "%dx%d" % FULL, "X-Frame-Format": "RGB565"})

    # ---- 照明付きの撮影 -------------------------------------------------------

    @app.post("/shot")
    async def shot(req: Request):
        qp = req.query_params
        try:
            q = max(1, min(int(qp.get("q", board.cfg["shot_q"])), 100))   # 既定はボードの撮影設定
        except ValueError:
            q = 1                                            # 実機は atoi
        light = qp["light"] != "0" if "light" in qp else board.cfg["shot_light"]
        if not board.camera:
            return err(503, "no camera")
        sid = board.request_shot(q, light, "http")
        if sid is None:
            return err(409, "shot queue full")
        return JSONResponse({"id": sid}, status_code=202)

    @app.get("/shot/info")
    def shot_info():
        return board.shot_info()

    @app.get("/shot.jpg")
    async def shot_jpg(id: int | None = None):
        s = next((s for s in reversed(board.shots) if id is None or s["id"] == id), None)
        if s is None:
            return err(404, "no such shot")
        return await send(s["jpeg"], "image/jpeg", {"X-Shot-Id": str(s["id"])})

    # ---- 照明 ---------------------------------------------------------------

    @app.get("/io")
    def io_get():
        return board.io()

    @app.post("/io")
    def io_post(req: Request):
        qp = req.query_params
        if "out" in qp:
            try:
                out = int(qp["out"])
            except ValueError:
                out = -1
            if not 0 <= out <= 255:
                return err(400, "out must be 0..255")
            mask, value = 0xFF, out
        elif "light" in qp:
            if qp["light"] not in ("0", "1"):
                return err(400, "light must be 0 or 1")
            mask, value = LIGHT_MASK, LIGHT_MASK if qp["light"] == "1" else 0
        elif "ch" in qp:
            try:
                ch = int(qp["ch"])
            except ValueError:
                ch = -1
            if not 0 <= ch <= 7:
                return err(400, "ch must be 0..7")
            if qp.get("on") not in ("0", "1"):
                return err(400, "on must be 0 or 1")
            mask, value = 1 << ch, (1 << ch) if qp["on"] == "1" else 0
        else:
            return err(400, "give out=, light=, or ch= and on=")
        if not board.light_board:
            return err(503, "MCP23017 not responding")
        board.out = (board.out & ~mask) | (value & mask)
        return board.io()

    # ---- 診断 ---------------------------------------------------------------

    @app.get("/stats")
    def stats():
        s = board.stats
        return PlainTextResponse(
            f"last={s['last']}\nbytes={s['bytes']}\nencode_ms={s['encode_ms']}\nsend_ms={s['send_ms']}\n"
            f"mbit_s={s['mbit_s']}\nframes_published={board.frames}\nheap_free=337211\npsram_free=16985832\n")

    @app.get("/bench")
    async def bench():
        block = bytes(range(256)) * 32                       # 8 KB
        total = 8 * 1024 * 1024

        async def gen():
            for _ in range(total // len(block)):
                yield block
                if opts.slow:
                    await asyncio.sleep(len(block) / SERIAL_BPS)
        return StreamingResponse(gen(), media_type="application/octet-stream")

    # ---- プリセット（実機は presets.json） ----------------------------------------

    # 向き（hflip / vflip）は含めない: 読み込みでボードは再起動しない（実機 2026-11-16〜）
    PRESET_KEYS = ("exposure_mode", "gain_mode", "exposure_us", "gain_index", "focus_mode", "focus_pos",
                   "wb_mode", "wb_red", "wb_blue", "sp", "shot_q", "shot_light")

    def name_ok(n: str | None) -> bool:
        return (n is not None and 0 < len(n.encode()) <= PRESET_NAME_MAX
                and not any(ch in '"\\' or ord(ch) < 0x20 or ord(ch) == 0x7F for ch in n))

    def preset_list() -> dict:
        return {"active": board.preset, "modified": bool(board.preset and board.preset_modified),
                "max": PRESETS_MAX, "presets": board.presets}

    def find(name: str) -> int:
        return next((i for i, p in enumerate(board.presets) if p["name"] == name), -1)

    @app.get("/presets")
    async def presets_get():
        return preset_list() if board.camera else not_found()

    @app.post("/presets/save")
    async def presets_save(name: str | None = None):
        if not board.camera:
            return not_found()
        if not name_ok(name):
            return err(400, BAD_NAME)
        i = find(name)
        if i < 0 and len(board.presets) >= PRESETS_MAX:
            return err(409, "too many presets")
        st = board.state()
        c = board.cfg
        p = {"name": name, **{k: st[k] for k in ("exposure_mode", "gain_mode", "focus_mode", "wb_mode",
                                                  "shot_q", "shot_light")},
             "exposure_us": lines_to_us(c["exp_lines"]), "gain_index": c["gain_index"],
             "focus_pos": c["focus_pos"], "wb_red": c["wb_red"], "wb_blue": c["wb_blue"], "sp": c["sp"]}
        if i >= 0:
            board.presets[i] = p
        else:
            board.presets.append(p)
        board.preset, board.preset_modified = name, False
        return preset_list()

    @app.post("/presets/load")
    async def presets_load(name: str | None = None):
        if not board.camera:
            return not_found()
        if not name_ok(name):
            return err(400, BAD_NAME)
        i = find(name)
        if i < 0:
            return err(404, "no such preset")
        v = {k: board.presets[i][k] for k in PRESET_KEYS if k in board.presets[i]}
        board.apply(v, force_af=True)              # 新しい試験片: auto ならピントを合わせ直す。向きは無いので再起動しない
        board.preset, board.preset_modified = name, False
        return board.state()

    @app.post("/presets/delete")
    async def presets_delete(name: str | None = None):
        if not board.camera:
            return not_found()
        if not name_ok(name):
            return err(400, BAD_NAME)
        i = find(name)
        if i < 0:
            return err(404, "no such preset")
        del board.presets[i]
        if board.preset == name:
            board.preset, board.preset_modified = None, False
        return preset_list()

    @app.post("/presets/rename")
    async def presets_rename(name: str | None = None, to: str | None = None):
        if not board.camera:
            return not_found()
        if not name_ok(name):
            return err(400, BAD_NAME)
        if not name_ok(to):
            return err(400, "to: " + BAD_NAME)
        i, j = find(name), find(to)
        if i < 0:
            return err(404, "no such preset")
        if j >= 0 and j != i:
            return err(409, "a preset with that name exists")
        board.presets[i]["name"] = to
        if board.preset == name:
            board.preset = to
        return preset_list()

    # ---- レシピ（実機は recipes.json。common/camcore/recipe.c と同じ検査・同じ文言） ----------

    RECIPES_MAX, RECIPE_STEPS_MAX, RECIPE_BODY_MAX = 20, 30, 12 * 1024
    MODE_KEYS = ("exposure_mode", "gain_mode", "focus_mode", "wb_mode")
    INT_KEYS = ("exposure_us", "gain_index", "focus_pos", "wb_red", "wb_blue")

    def is_int(x, lo: int, hi: int) -> bool:
        return (isinstance(x, (int, float)) and not isinstance(x, bool)
                and float(x).is_integer() and lo <= x <= hi)

    def check_step(st) -> tuple[str | None, dict | None]:
        if not isinstance(st, dict):
            return "each step must be an object", None
        if any(k not in ("enabled", "overrides", "light_out", "q", "settle_ms") for k in st):
            return "step: unknown key (allowed: enabled, overrides, light_out, q, settle_ms)", None
        out: dict = {}
        en = st.get("enabled")
        if "enabled" in st and not isinstance(en, bool):
            return "enabled must be true or false", None
        out["enabled"] = en is not False
        ov = st.get("overrides")
        n: dict = {}
        if ov is not None:
            if not isinstance(ov, dict):
                return "overrides must be an object", None
            for k, x in ov.items():
                if k in MODE_KEYS:
                    if x not in ("auto", "manual"):
                        return 'overrides: *_mode must be "auto" or "manual"', None
                    n[k] = x
                elif k in INT_KEYS:
                    if not is_int(x, 0, 100000000):
                        return "overrides: values must be integers >= 0", None
                    n[k] = int(x)
                else:
                    return ("overrides: unknown key (allowed: exposure_mode, exposure_us, gain_mode, gain_index, "
                            "focus_mode, focus_pos, wb_mode, wb_red, wb_blue)"), None
        out["overrides"] = n
        lo = st.get("light_out")
        if lo is not None and not is_int(lo, 0, 255):
            return "light_out must be 0..255 or null", None
        out["light_out"] = None if lo is None else int(lo)
        if "q" in st and not is_int(st["q"], 1, 100):
            return "q must be 1..100", None
        out["q"] = int(st.get("q", 95))
        if "settle_ms" in st and not is_int(st["settle_ms"], 0, 60000):
            return "settle_ms must be 0..60000", None
        out["settle_ms"] = int(st.get("settle_ms", 800))
        return None, out

    def check_recipe(r) -> tuple[str | None, dict | None]:
        """保存する形（省略キーを既定値で埋める）に整える。実機の check_recipe() と同じ順で検査する。"""
        if not isinstance(r, dict):
            return "body must be a JSON object", None
        if any(k not in ("name", "base_preset", "steps") for k in r):
            return "unknown key (allowed: name, base_preset, steps)", None
        name = r.get("name")
        if not isinstance(name, str) or not name_ok(name):
            return BAD_NAME, None
        base = r.get("base_preset")
        if base is not None and (not isinstance(base, str) or not name_ok(base)):
            return "base_preset must be a preset name or null", None
        steps = r.get("steps")
        if not isinstance(steps, list) or not steps:
            return "steps must be a non-empty array", None
        if len(steps) > RECIPE_STEPS_MAX:
            return "too many steps (max 30)", None
        out = []
        for st in steps:
            e, n = check_step(st)
            if e:
                return e, None
            out.append(n)
        return None, {"name": name, "base_preset": base, "steps": out}

    def recipe_list() -> dict:
        return {"max": RECIPES_MAX, "max_steps": RECIPE_STEPS_MAX, "recipes": board.recipes}

    def find_recipe(name: str) -> int:
        return next((i for i, r in enumerate(board.recipes) if r["name"] == name), -1)

    @app.get("/recipes")
    async def recipes_get():
        return recipe_list() if board.camera else not_found()

    @app.post("/recipes/save")
    async def recipes_save(req: Request):
        if not board.camera:
            return not_found()
        body = await req.body()
        if not body:
            return err(400, "a JSON body is required")
        if len(body) > RECIPE_BODY_MAX:
            return err(413, "recipe too large")
        try:
            r = json.loads(body)
        except ValueError:
            return err(400, "body is not JSON")
        e, n = check_recipe(r)
        if e:
            return err(400, e)
        i = find_recipe(n["name"])
        if i < 0 and len(board.recipes) >= RECIPES_MAX:
            return err(409, "too many recipes")
        if i >= 0:
            board.recipes[i] = n
        else:
            board.recipes.append(n)
        return recipe_list()

    @app.post("/recipes/delete")
    async def recipes_delete(name: str | None = None):
        if not board.camera:
            return not_found()
        if not name_ok(name):
            return err(400, BAD_NAME)
        i = find_recipe(name)
        if i < 0:
            return err(404, "no such recipe")
        del board.recipes[i]
        return recipe_list()

    @app.post("/recipes/rename")
    async def recipes_rename(name: str | None = None, to: str | None = None):
        if not board.camera:
            return not_found()
        if not name_ok(name):
            return err(400, BAD_NAME)
        if not name_ok(to):
            return err(400, "to: " + BAD_NAME)
        i, j = find_recipe(name), find_recipe(to)
        if i < 0:
            return err(404, "no such recipe")
        if j >= 0 and j != i:
            return err(409, "a recipe with that name exists")
        board.recipes[i]["name"] = to
        return recipe_list()

    # ---- ダミー専用 -----------------------------------------------------------

    def mock_state() -> dict:
        return {"camera": board.camera, "light_board": board.light_board, "slow": opts.slow,
                "unplugged": board.unplugged, "down": board.down, "boot": board.boot,
                "button_every": opts.button_every, "state_file": opts.state_from}

    # ---- usb-app の PC サーバー独自の API（server.py と同じ）: レシピの結果を PC に保存 ----
    save_dir = None if opts.no_save else opts.save_dir.expanduser().resolve()
    save_part = re.compile(r'[^\\/:*?"<>|\x00-\x1f]{1,120}')

    def part_ok(x: str) -> bool:
        return bool(save_part.fullmatch(x)) and x.strip(" .") != "" and x == x.strip()

    @app.get("/pc/info")
    def pc_info():
        return {"server": "usb-app", "save_dir": str(save_dir) if save_dir else None}

    @app.post("/pc/save")
    async def pc_save(req: Request, folder: str, name: str):
        if save_dir is None:
            return JSONResponse({"error": "saving is off (server started with --no-save)"}, status_code=404)
        if not (part_ok(folder) and part_ok(name)) or not name.lower().endswith((".jpg", ".json")):
            return JSONResponse({"error": "bad folder or file name"}, status_code=400)
        data = await req.body()
        if not data or len(data) > 32 * 1024 * 1024:
            return JSONResponse({"error": "empty or too large"}, status_code=400 if not data else 413)
        path = save_dir / folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return {"path": str(path), "size": len(data)}

    @app.get("/_mock", response_class=HTMLResponse)
    def mock_panel():
        return HTMLResponse((HERE / "panel.html").read_text(encoding="utf-8"),
                            headers={"Cache-Control": "no-store"})

    @app.get("/_mock/state")
    def mock_get():
        return mock_state()

    @app.post("/_mock/state")
    def mock_set(camera: int | None = None, light_board: int | None = None, slow: int | None = None):
        if camera is not None:
            board.camera = bool(camera)
        if light_board is not None:
            board.light_board = bool(light_board)
            if not board.light_board:
                board.out = 0
        if slow is not None:
            opts.slow = bool(slow)
        return mock_state()

    @app.post("/_mock/button")
    async def mock_button():
        """物理ボタン: 照明を点けて撮る（実機の BUTTON_QUALITY は 95）。"""
        sid = board.request_shot(board.cfg["shot_q"], board.cfg["shot_light"], "button")
        if sid is None:
            return err(409, "shot queue full")
        return JSONResponse({"id": sid}, status_code=202)

    @app.post("/_mock/unplug")
    async def mock_unplug(on: int = 1):
        """USB を抜く（on=1）/ 挿す（on=0）。挿すとボードは起動し直す（実機と同じく写真は消える）。"""
        if bool(on) != board.unplugged:
            board.unplugged = bool(on)
            if not board.unplugged:
                board.boot_up()
        return mock_state()

    @app.post("/_mock/reboot")
    async def mock_reboot():
        board.reboot(delay=0)
        return mock_state()

    @app.api_route("/{path:path}", methods=["GET", "POST"])
    def other(path: str):
        return not_found()

    return app


def capture(base: str, out: Path) -> None:
    """実機（usb-app の PC サーバー、または wifi-app のボード）から中身を写す。読むだけで何も変えない。"""
    def get(path: str):
        try:
            with urllib.request.urlopen(base + path, timeout=20) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None                        # カメラ無しのボードには /ctrl などが無い
            raise SystemExit(f"{path}: {e.code} {e.read()[:200].decode(errors='replace')}")
        except OSError as e:
            raise SystemExit(f"{base}{path} に届きません: {e}")
    ctrl = get("/ctrl")
    io_ = get("/io") or {}
    d = {
        "source": base, "captured_at": datetime.now().isoformat(timespec="seconds"),
        "camera": ctrl is not None, "light_board": bool(io_.get("present")),
        "ctrl": ctrl,
        "presets": (get("/presets") or {}).get("presets", []) if ctrl else [],
        "recipes": (get("/recipes") or {}).get("recipes", []) if ctrl else [],
    }
    out.write_text(json.dumps(d, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{out} に書きました: カメラ{'あり' if d['camera'] else 'なし'}、照明ボード{'あり' if d['light_board'] else 'なし'}、"
          f"プリセット {len(d['presets'])} 件、レシピ {len(d['recipes'])} 件")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="0.0.0.0", help="待ち受けアドレス（既定: 全インターフェース）")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--slow", action="store_true", help="シリアルの遅さを真似る（約 200 kB/s、応答に遅れ）")
    ap.add_argument("--no-camera", action="store_true", help="カメラ無しのボードとして振る舞う")
    ap.add_argument("--no-light-board", action="store_true", help="照明ボード（MCP23017）無し")
    ap.add_argument("--fps", type=float, default=10, help="プレビューの fps（既定 10）")
    ap.add_argument("--button-every", type=float, default=0, metavar="SEC",
                    help="SEC 秒ごとに物理ボタンを押したことにする")
    ap.add_argument("--cors", action="store_true",
                    help="Access-Control-Allow-Origin: * を付ける（実機のサーバーは付けない）")
    ap.add_argument("--save-dir", type=Path, default=HERE / "captures",
                    help="レシピの結果を保存するフォルダ（既定: このフォルダの captures/。実行ごとにフォルダを作る）")
    ap.add_argument("--no-save", action="store_true", help="PC に保存しない（画面はダウンロードに切り替わる）")
    ap.add_argument("--state", type=Path, default=STATE_FILE, metavar="FILE",
                    help="起動時の中身（プリセット・レシピ・設定）。既定: このフォルダの board_state.json")
    ap.add_argument("--empty", action="store_true", help="中身を読まず、空のボードで始める")
    ap.add_argument("--capture", metavar="URL",
                    help="実機の PC サーバー（例 http://127.0.0.1:8080）から中身を --state に写して終わる")
    a = ap.parse_args()
    if a.capture:
        capture(a.capture.rstrip("/"), a.state)
        return
    if not WEBUI.exists():
        raise SystemExit(f"Web 画面が見つかりません: {WEBUI}")
    board = Board(camera=not a.no_camera, light_board=not a.no_light_board)
    a.state_from = None
    if not a.empty and a.state.exists():
        d = json.loads(a.state.read_text(encoding="utf-8"))
        board.load_state(d)
        board.camera = board.camera and not a.no_camera
        board.light_board = board.light_board and not a.no_light_board
        a.state_from = str(a.state)
        print(f"中身: {a.state}（{d.get('source', '?')}、{d.get('captured_at', '?')}）"
              f" プリセット {len(board.presets)} 件、レシピ {len(board.recipes)} 件", flush=True)
    else:
        print("中身: 空のボード", flush=True)
    print(f"usb-app mock: http://127.0.0.1:{a.port}/   操作パネル: http://127.0.0.1:{a.port}/_mock", flush=True)
    uvicorn.run(build_app(board, a), host=a.host, port=a.port, log_level="warning",
                timeout_graceful_shutdown=2)


if __name__ == "__main__":
    main()
