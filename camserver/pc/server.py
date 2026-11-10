"""PC 側 FastAPI サーバー: ブラウザ UI と API からボードのシャッター・カメラ設定を操作する。

  BOARD_HOST=172.20.10.6 pc/run.sh        # http://127.0.0.1:8080
  python pc/server.py --board 172.20.10.6 [--host 127.0.0.1] [--port 8080]
  python pc/server.py shot --board 172.20.10.6 -q 95      # UI なしで 1 枚撮る

ボードは camctl 入りのファーム (firmware/) を書き込んでおく。設定はボードの LittleFS に保存される。
API は /docs (OpenAPI) で見られる。`board` クエリを省くと --board / BOARD_HOST の値を使う。
"""
import argparse
import os
import sys
import urllib.request
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

import board as bd

HERE = Path(__file__).parent
CAPTURE_DIR = Path(os.getenv("CAPTURE_DIR", HERE / "captures"))


def build_app(default_board: str) -> FastAPI:
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    app = FastAPI(title="esp32p4-camserver")

    def pick(board: str | None) -> str:
        return board or default_board

    def relay(board: str | None, method: str, path: str, params: dict | None = None):
        try:
            return bd.call(pick(board), method, path, params)
        except bd.BoardError as e:
            raise HTTPException(e.status, f"ボード: {e.detail}")
        except bd.NET_ERRORS as e:
            raise HTTPException(502, f"ボードに接続できません: {e}")
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.get("/", response_class=HTMLResponse)
    def index():
        return (HERE / "static" / "index.html").read_text().replace("__BOARD__", default_board)

    @app.post("/api/shutter")
    def shutter(board: str | None = None, q: int = 95):
        try:
            return bd.take_shot(pick(board), q, CAPTURE_DIR)
        except bd.NET_ERRORS as e:
            raise HTTPException(502, f"ボードに接続できません: {e}")
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.get("/api/status")
    def status(board: str | None = None):
        """ボードに届くか、設定が保存から復元されたか (config: loaded / default / error)。"""
        try:
            s = bd.call(pick(board), "GET", "/ctrl")
        except bd.NET_ERRORS as e:
            return {"reachable": False, "error": str(e)}
        except bd.BoardError as e:
            return {"reachable": True, "error": e.detail}
        except ValueError as e:
            raise HTTPException(400, str(e))
        return {"reachable": True, "config": s.get("config"), "ctrl": s}

    @app.get("/api/ctrl/caps")
    def ctrl_caps(board: str | None = None):
        return relay(board, "GET", "/ctrl/caps")

    @app.get("/api/ctrl")
    def ctrl_state(board: str | None = None):
        return relay(board, "GET", "/ctrl")

    @app.post("/api/ctrl")
    def ctrl_set(request: Request, board: str | None = None):
        params = {k: v for k, v in request.query_params.items() if k != "board"}
        bad = sorted(set(params) - set(bd.CTRL_KEYS))
        if bad:
            raise HTTPException(400, f"使えないキー: {', '.join(bad)}")
        return relay(board, "POST", "/ctrl", params)

    @app.post("/api/af")
    def af(board: str | None = None):
        return relay(board, "POST", "/af")

    @app.post("/api/wb/onepush")
    def wb_onepush(board: str | None = None):
        return relay(board, "POST", "/wb/onepush")

    @app.post("/api/config/reset")
    def config_reset(board: str | None = None):
        return relay(board, "POST", "/config/reset")

    @app.get("/api/preview.mjpg")
    def preview(board: str | None = None, q: int = 50, n: int = 8):
        """ボード :81 の MJPEG をそのまま中継する (ブラウザがボードに直接届かない環境向け)。"""
        host = bd.check_host(pick(board)).split(":")[0]
        try:
            up = urllib.request.urlopen(f"http://{host}:81/stream.mjpg?q={q}&n={n}", timeout=bd.CTRL_TIMEOUT_S)
        except bd.NET_ERRORS as e:
            raise HTTPException(502, f"ボードに接続できません: {e}")

        def chunks():
            with up:
                while data := up.read(16384):
                    yield data

        return StreamingResponse(chunks(), media_type=up.headers.get("Content-Type", "multipart/x-mixed-replace"))

    @app.get("/api/captures")
    def captures():
        return sorted((p.name for p in CAPTURE_DIR.glob("*.jpg")), reverse=True)

    app.mount("/captures", StaticFiles(directory=CAPTURE_DIR), name="captures")
    return app


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", choices=["serve", "shot"], default="serve")
    ap.add_argument("--board", default=os.getenv("BOARD_HOST", ""), help="ボードの IP / ホスト名")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("-q", "--quality", type=int, default=95, help="shot: JPEG 品質")
    a = ap.parse_args()

    if a.cmd == "shot":
        try:
            r = bd.take_shot(a.board, a.quality, CAPTURE_DIR)
        except Exception as e:
            sys.exit(f"失敗: {e}")
        print(f"{CAPTURE_DIR / r['file']}  {r['width']}x{r['height']}  {r['bytes']} bytes  {r['seconds']}s")
        return

    import uvicorn
    print(f"http://{a.host}:{a.port}  (ボード: {a.board or '未設定'})")
    uvicorn.run(build_app(a.board), host=a.host, port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
