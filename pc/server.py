"""PC 側サーバ (最小実装)

- POST /upload        : ESP32-P4 から JPEG を受け取り captures/ に保存
                        (body は raw JPEG, Content-Type: image/jpeg)
- POST /capture/{id}  : MQTT で cam/<id>/cmd に {"action":"capture"} を publish
- GET  /latest        : 最新の画像を返す
- GET  /              : 簡易 Web UI

環境変数: MQTT_HOST (既定 localhost), MQTT_PORT (既定 1883)
"""
import json
import os
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse

CAPTURE_DIR = Path(__file__).parent / "captures"
CAPTURE_DIR.mkdir(exist_ok=True)

MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

app = FastAPI(title="esp32p4-cam")
mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)


@app.on_event("startup")
def _startup() -> None:
    mq.connect_async(MQTT_HOST, MQTT_PORT)
    mq.loop_start()


@app.on_event("shutdown")
def _shutdown() -> None:
    mq.loop_stop()
    mq.disconnect()


@app.post("/upload")
async def upload(request: Request, cam: str = "unknown"):
    data = await request.body()
    if not data.startswith(b"\xff\xd8"):
        raise HTTPException(400, "not a JPEG")
    name = f"{cam}_{datetime.now():%Y%m%d_%H%M%S_%f}.jpg"
    (CAPTURE_DIR / name).write_bytes(data)
    return {"saved": name, "bytes": len(data)}


@app.post("/capture/{cam_id}")
def capture(cam_id: str):
    info = mq.publish(f"cam/{cam_id}/cmd", json.dumps({"action": "capture"}), qos=1)
    return {"published": info.rc == mqtt.MQTT_ERR_SUCCESS}


@app.get("/latest")
def latest():
    files = sorted(CAPTURE_DIR.glob("*.jpg"), key=lambda p: p.stat().st_mtime)
    if not files:
        raise HTTPException(404, "no captures yet")
    return FileResponse(files[-1], media_type="image/jpeg")


@app.get("/", response_class=HTMLResponse)
def index():
    return """<!doctype html><meta charset="utf-8"><title>esp32p4-cam</title>
<h1>esp32p4-cam</h1>
<input id="cam" value="p4cam01"> <button onclick="cap()">撮影</button>
<p><img id="img" style="max-width:100%"></p>
<script>
async function cap(){
  await fetch('/capture/'+document.getElementById('cam').value,{method:'POST'});
  setTimeout(()=>{document.getElementById('img').src='/latest?'+Date.now()},2000);
}
</script>"""
