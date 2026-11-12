"""usb-app PC server: the board over a USB serial tunnel, served to the LAN.

    .venv/bin/python server.py [--serial /dev/cu.usbmodem…] [--port 8080]

Open the printed URL from this PC or any other on the network. The page is the
one the board serves itself under wifi-app (common/webui/index.html), and every
API path (/ctrl, /af, /io, /snapshot.jpg, …) is passed through to the board
unchanged - this server adds no API of its own. Photos download in the
browser, as with wifi-app; nothing is stored here.

What it does add, because a serial link is slow (~200 kB/s at 2 Mbaud):
  - /stream.mjpg takes ONE stream from the board and hands every frame to all
    viewers, instead of one board stream per viewer. A viewer asking for a
    different size or quality restarts it with theirs.
  - While a still is on its way (/snapshot.jpg and the like) the preview stops,
    so the still gets the whole link.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import re
import socket
import sys
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, StreamingResponse
from serial.tools import list_ports

from httpch import BoardError, request
from tunnel import Tunnel

HERE = Path(__file__).resolve().parent
WEBUI = HERE.parent.parent / "common" / "webui" / "index.html"

API_PORT, MJPEG_PORT = 80, 81
# The board's :80 server takes 4 sockets (common/camcore/camcore.c) and purges
# the oldest past that; keep one spare.
API_SLOTS = 3
# Big transfers: the preview pauses while one of these is on the link.
BIG_PATHS = {"snapshot.jpg", "snapshot.raw", "shot.jpg", "bench"}
PASS_HEADERS = ("content-type", "content-length", "cache-control", "x-frame-size", "x-frame-format")


def log(msg: str) -> None:
    print(msg, flush=True)


class MjpegHub:
    """One upstream MJPEG stream from the board, fanned out to every viewer."""

    def __init__(self, tunnel: Tunnel):
        self.tunnel = tunnel
        self.viewers: set[asyncio.Queue[bytes]] = set()
        self.params: tuple[int, int] | None = None   # (q, n) of the upstream
        self.task: asyncio.Task | None = None
        self.pauses = 0

    def _restart(self) -> None:
        if self.task:
            self.task.cancel()
            self.task = None
        if self.viewers and not self.pauses and self.params:
            self.task = asyncio.create_task(self._upstream(*self.params))

    async def _upstream(self, q: int, n: int) -> None:
        while self.viewers:
            try:
                rep = await request(self.tunnel, MJPEG_PORT, "GET", f"/stream.mjpg?q={q}&n={n}")
                try:
                    if rep.status != 200:
                        raise BoardError(f"stream answered {rep.status}")
                    buf = bytearray()
                    async with contextlib.aclosing(rep.body()) as body:
                        async for chunk in body:
                            buf += chunk
                            for jpeg in self._frames(buf):
                                self._publish(jpeg)
                finally:
                    rep.close()
            except BoardError as e:
                log(f"[mjpeg] {e} - retrying")
            await asyncio.sleep(1)

    @staticmethod
    def _frames(buf: bytearray):
        """Complete JPEGs off the front of the board's multipart body."""
        while True:
            end = buf.find(b"\r\n\r\n")
            if end < 0:
                return
            m = re.search(rb"Content-Length:\s*(\d+)", buf[:end], re.I)
            if not m:
                del buf[:end + 4]
                continue
            start, size = end + 4, int(m.group(1))
            if len(buf) < start + size:
                return
            jpeg = bytes(buf[start:start + size])
            del buf[:start + size]
            yield jpeg

    def _publish(self, jpeg: bytes) -> None:
        for v in self.viewers:
            if v.full():                 # a slow viewer skips frames, never lags
                v.get_nowait()
            v.put_nowait(jpeg)

    async def watch(self, q: int, n: int):
        """Multipart body for one viewer."""
        mine: asyncio.Queue[bytes] = asyncio.Queue(maxsize=1)
        self.viewers.add(mine)
        if (q, n) != self.params or self.task is None:
            self.params = (q, n)
            self._restart()
        try:
            while True:
                jpeg = await mine.get()
                yield (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg)
                       + jpeg + b"\r\n")
        finally:
            self.viewers.discard(mine)
            if not self.viewers:
                self._restart()          # stops the upstream

    @contextlib.asynccontextmanager
    async def paused(self):
        self.pauses += 1
        task, self.task = self.task, None
        if task:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task
        try:
            yield
        finally:
            self.pauses -= 1
            if not self.pauses:
                self._restart()


def build_app(serial_port: str, baud: int, debug: bool = False) -> FastAPI:
    tunnel = Tunnel(serial_port, baud, log=log, debug=debug)
    hub = MjpegHub(tunnel)
    slots = asyncio.Semaphore(API_SLOTS)

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        tunnel.start()
        yield
        tunnel.stop()

    app = FastAPI(title="usb-app", lifespan=lifespan, docs_url=None, redoc_url=None)

    @app.get("/", response_class=HTMLResponse)
    def index():
        # The page builds its preview URL from window.STREAM_URL when set; ours
        # is this server's fan-out, on the same origin.
        html = WEBUI.read_text(encoding="utf-8")
        inject = '<script>window.STREAM_URL="/stream.mjpg"</script>\n<script>'
        return HTMLResponse(html.replace("<script>", inject, 1), headers={"Cache-Control": "no-store"})

    @app.get("/stream.mjpg")
    async def stream(q: int = 50, n: int = 8):
        return StreamingResponse(hub.watch(max(1, min(q, 100)), n),
                                 media_type="multipart/x-mixed-replace; boundary=frame",
                                 headers={"Cache-Control": "no-store"})

    @app.api_route("/{path:path}", methods=["GET", "POST"])
    async def board(path: str, req: Request):
        target = "/" + path + (f"?{req.url.query}" if req.url.query else "")
        body = await req.body()
        held = contextlib.AsyncExitStack()
        try:
            if req.method == "GET" and path in BIG_PATHS:
                await held.enter_async_context(hub.paused())
            await held.enter_async_context(slots)
            rep = await request(tunnel, API_PORT, req.method, target, body)
        except BoardError as e:
            await held.aclose()
            return JSONResponse({"error": f"board: {e}"}, status_code=503)
        except BaseException:
            await held.aclose()
            raise

        async def relay():
            try:
                async with contextlib.aclosing(rep.body()) as chunks:
                    async for chunk in chunks:
                        yield chunk
            except BoardError as e:
                log(f"[api] {target}: {e}")
            finally:
                rep.close()
                await held.aclose()

        headers = {k: v for k, v in rep.headers.items() if k in PASS_HEADERS}
        return StreamingResponse(relay(), status_code=rep.status, headers=headers)

    return app


def find_serial() -> str | None:
    """The board's CH343 (WCH, VID 0x1A86), or the only USB serial port there is."""
    ports = list(list_ports.comports())
    wch = [p.device for p in ports if p.vid == 0x1A86]
    if len(wch) == 1:
        return wch[0]
    usb = [p.device for p in ports if p.vid is not None]
    return usb[0] if len(usb) == 1 else None


def lan_addresses() -> list[str]:
    """This PC's IPv4 addresses that others on the LAN can use."""
    addrs = set()
    with contextlib.suppress(OSError):
        # A UDP "connect" picks the outgoing interface without sending anything.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            addrs.add(s.getsockname()[0])
    with contextlib.suppress(OSError):
        addrs.update(socket.gethostbyname_ex(socket.gethostname())[2])
    return sorted(a for a in addrs if not a.startswith("127."))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", help="the board's serial port (default: find the CH343)")
    ap.add_argument("--baud", type=int, default=2_000_000, help="must match the firmware (default 2000000)")
    ap.add_argument("--host", default="0.0.0.0", help="listen address (default: every interface)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--debug", action="store_true", help="log every tunnel channel")
    a = ap.parse_args()

    port = a.serial or find_serial()
    if not port:
        sys.exit("no serial port found - give one with --serial (see: python -m serial.tools.list_ports)")
    if not WEBUI.exists():
        sys.exit(f"web UI not found at {WEBUI}")

    log(f"board on {port} at {a.baud} baud")
    if a.host in ("0.0.0.0", ""):
        for ip in lan_addresses() or ["<this PC's IP>"]:
            log(f"  http://{ip}:{a.port}/")
    log(f"  http://127.0.0.1:{a.port}/")
    uvicorn.run(build_app(port, a.baud, a.debug), host=a.host, port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
