"""esp32p4-cam: 低解像度プレビュー + 露出・ピント操作 + シャッターでフルサイズ保存

ボード側は esp32-p4-imx-camera の examples/imx708_wifi_control を書き込んでおく:
  :81/stream.mjpg?q=<1-100>&n=<16|12|8|4>   プレビュー (MJPEG, n は縮小率 n/16)
  :80/snapshot.jpg?q=<1-100>                フルサイズ静止画 (1920x1080)
  :80/ctrl, /ctrl/caps, /af, /wb/onepush, /config/reset
                                            露出・ゲイン・ピント・ホワイトバランスの操作 (docs/SPEC-camera-controls.md)
imx708_wifi_snapshot でもプレビューと撮影はできる (操作欄は無効になる)。

使い方:
  python pc/camtool.py serve --board 172.20.10.6        # ブラウザ UI (http://127.0.0.1:8080)
  python pc/camtool.py shot  --board 172.20.10.6 -q 95  # シャッターだけ (UI なし)

保存先は pc/captures/YYYYmmdd_HHMMSS_mmm.jpg (既定)。依存: fastapi, uvicorn (requirements.txt)。
"""
import argparse
import json
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

CAPTURE_DIR = Path(os.getenv("CAPTURE_DIR", Path(__file__).parent / "captures"))
HOST_RE = re.compile(r"^[A-Za-z0-9.\-]{1,253}(:\d{1,5})?$")  # 実機はポート 80 固定。:port はテスト用
SHOT_TIMEOUT_S = 40  # 低速リンクでは 480 KB に数秒かかる
CTRL_TIMEOUT_S = 5
CTRL_KEYS = ("exposure_mode", "gain_mode", "exposure_us", "gain_index", "focus_mode", "focus_pos",
             "wb_mode", "wb_red", "wb_blue")


class BoardError(Exception):
    """ボードがエラーを返した (status はそのまま中継する)。"""

    def __init__(self, status: int, detail: str):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def board_call(board: str, method: str, path: str, params: dict | None = None):
    """ボードの制御 API を呼んで JSON を返す。"""
    if not HOST_RE.match(board):
        raise ValueError(f"ボードのアドレスが不正: {board!r}")
    url = f"http://{board}{path}"
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


def take_shot(board: str, quality: int, out_dir: Path = CAPTURE_DIR) -> dict:
    """ボードからフルサイズ静止画を取得して保存する。"""
    if not HOST_RE.match(board):
        raise ValueError(f"ボードのアドレスが不正: {board!r}")
    quality = max(1, min(100, int(quality)))
    out_dir.mkdir(parents=True, exist_ok=True)
    url = f"http://{board}/snapshot.jpg?q={quality}"
    t0 = time.time()
    with urllib.request.urlopen(url, timeout=SHOT_TIMEOUT_S) as r:
        data = r.read()
    if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
        raise ValueError(f"JPEG として不完全 ({len(data)} bytes)")
    now = datetime.now()
    name = f"{now:%Y%m%d_%H%M%S}_{now.microsecond // 1000:03d}.jpg"  # 連写で上書きしない
    (out_dir / name).write_bytes(data)
    size = jpeg_size(data)
    return {"file": name, "bytes": len(data), "seconds": round(time.time() - t0, 2),
            "width": size[0] if size else None, "height": size[1] if size else None}


PAGE = """<!doctype html><html lang="ja"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>esp32p4-cam</title>
<style>
 :root{color-scheme:dark light;--bg:#14161a;--fg:#e8e8ea;--mut:#8b8f98;--card:#1d2026;--acc:#4da3ff;--ok:#4cc38a;--bad:#ff6b6b}
 @media (prefers-color-scheme:light){:root{--bg:#f4f5f7;--fg:#1b1d22;--mut:#667;--card:#fff;--acc:#1a73e8;--ok:#1e8e5a;--bad:#d33}}
 *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif}
 main{max-width:1200px;margin:0 auto;padding:16px;display:grid;grid-template-columns:1fr 300px;gap:16px}
 @media(max-width:860px){main{grid-template-columns:1fr}}
 .card{background:var(--card);border-radius:10px;padding:12px}
 #view{width:100%;aspect-ratio:16/9;background:#000;border-radius:8px;display:block;object-fit:contain}
 label{display:block;margin:10px 0 4px;color:var(--mut);font-size:12px}
 input,select{width:100%;padding:7px;border-radius:6px;border:1px solid #0003;background:var(--bg);color:var(--fg);font:inherit}
 input[type=range]{padding:0}
 button#shot{width:100%;margin-top:14px;padding:16px;font-size:18px;font-weight:600;border:0;border-radius:10px;background:var(--acc);color:#fff;cursor:pointer}
 button#shot:disabled{opacity:.6;cursor:wait}
 #msg{margin-top:8px;min-height:2.6em;font-size:13px}.ok{color:var(--ok)}.bad{color:var(--bad)}
 #gal{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px}
 #gal a{display:block}#gal img{width:100%;aspect-ratio:16/9;object-fit:cover;border-radius:4px;display:block}
 h2{font-size:13px;margin:14px 0 0;color:var(--mut);font-weight:600}
 .hint{color:var(--mut);font-size:12px;margin-top:6px}
 #ctl{margin-top:14px;border-top:1px solid #8884;padding-top:4px}
 #ctl.off{opacity:.45;pointer-events:none}
 .row{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:12px}
 .row h3{font-size:13px;margin:0;font-weight:600}
 .val{font-variant-numeric:tabular-nums;color:var(--fg)}
 .sw{display:inline-flex;border:1px solid #8886;border-radius:7px;overflow:hidden}
 .sw button{border:0;background:transparent;color:var(--mut);padding:4px 10px;font:inherit;font-size:12px;cursor:pointer}
 .sw button.on{background:var(--acc);color:#fff}
 .chk{display:inline-flex;align-items:center;gap:4px;margin:0;color:var(--mut);font-size:12px}
 .chk input{width:auto}
 input[type=range]:disabled{opacity:.4}
 .af{display:flex;align-items:center;gap:8px;margin-top:8px}
 .af button{padding:5px 14px;border:1px solid var(--acc);border-radius:7px;background:transparent;color:var(--acc);font:inherit;cursor:pointer}
 .af button:disabled{opacity:.5;cursor:wait}
 #cmsg{min-height:1.4em;font-size:12px;margin-top:6px}
 .pv{position:relative}
 #wbbox{position:absolute;left:37.5%;top:37.5%;width:25%;height:25%;border:1px dashed #fff8;pointer-events:none;display:none}
 #reset{color:var(--mut);font-size:11px;background:none;border:0;padding:0;margin-top:8px;cursor:pointer;text-decoration:underline}
</style>
<main>
 <section class="card">
  <div class="pv"><img id="view" alt="preview"><div id="wbbox" title="白を合わせる範囲"></div></div>
  <div class="hint" id="pinfo"></div>
 </section>
 <aside class="card">
  <label for="board">ボードのアドレス</label>
  <input id="board" placeholder="例 172.20.10.6" spellcheck="false">
  <label for="n">プレビューのサイズ</label>
  <select id="n"><option value="4">480×264</option><option value="8" selected>960×536</option>
   <option value="12">1440×792</option><option value="16">1920×1080</option></select>
  <label for="pq">プレビューの画質 <span id="pqv"></span></label>
  <input id="pq" type="range" min="10" max="95" value="50">
  <label for="sq">シャッターの画質 <span id="sqv"></span></label>
  <input id="sq" type="range" min="50" max="100" value="100">
  <div id="ctl" class="off">
   <div class="row"><h3>露出</h3>
    <span class="sw" id="expsw"><button data-v="auto">Auto</button><button data-v="manual">Manual</button></span></div>
   <label for="exp">シャッター <span class="val" id="expv">–</span></label>
   <input id="exp" type="range" min="0" max="1000" value="500">
   <div class="row" style="margin-top:8px"><label for="gain" style="margin:0">明るさ (ゲイン) <span class="val" id="gainv">–</span></label>
    <label class="chk" title="シャッター優先: 明るさを自動で合わせる"><input id="gauto" type="checkbox" checked>自動</label></div>
   <input id="gain" type="range" min="0" max="46" value="0">
   <div class="row"><h3>ピント</h3>
    <span class="sw" id="focsw"><button data-v="auto">Auto</button><button data-v="manual">Manual</button></span></div>
   <label for="foc">位置 <span class="val" id="focv">–</span> <span style="font-size:11px">(大きいほど近く)</span></label>
   <input id="foc" type="range" min="0" max="1023" value="512">
   <div class="af"><button id="afbtn">AF</button><span class="hint" id="afv" style="margin:0"></span></div>
   <div id="wbsec">
   <div class="row"><h3>ホワイトバランス</h3>
    <span class="sw" id="wbsw"><button data-v="auto">Auto</button><button data-v="manual">Manual</button></span></div>
   <label for="wbr">赤 <span class="val" id="wbrv">–</span></label>
   <input id="wbr" type="range" min="300" max="3999" value="1500">
   <label for="wbb">青 <span class="val" id="wbbv">–</span></label>
   <input id="wbb" type="range" min="300" max="3999" value="1500">
   <div class="af"><button id="wbbtn" title="白い紙などを点線の枠に写して押す">白を合わせる</button><span class="hint" id="wbv" style="margin:0"></span></div>
   </div>
   <div id="cmsg"></div>
   <button id="reset">設定をリセット</button>
  </div>
  <button id="shot">シャッター (Space)</button>
  <div id="msg"></div>
  <h2>撮影した写真</h2><div id="gal"></div>
 </aside>
</main>
<script>
const $=id=>document.getElementById(id), LS=window.localStorage||{};
const get=(k,d)=>{try{return LS.getItem(k)??d}catch(e){return d}}, put=(k,v)=>{try{LS.setItem(k,v)}catch(e){}};
$('board').value=get('board','__BOARD__'); $('n').value=get('n','8'); $('pq').value=get('pq','50'); $('sq').value=get('sq','100');
function previewUrl(){return `http://${$('board').value.trim().split(':')[0]}:81/stream.mjpg?q=${$('pq').value}&n=${$('n').value}`}
function startPreview(){
  ['board','n','pq','sq'].forEach(k=>put(k,$(k).value));
  $('pqv').textContent=$('pq').value; $('sqv').textContent=$('sq').value;
  if(!$('board').value.trim()){$('pinfo').textContent='ボードのアドレスを入力してください';return}
  $('view').src=previewUrl(); $('pinfo').textContent='プレビュー中';
}
$('view').onerror=()=>{$('pinfo').textContent='プレビューに接続できません(アドレスと、PC とボードが同じネットワークか確認)'};
['board','n','pq'].forEach(k=>$(k).addEventListener('change',startPreview));
$('board').addEventListener('change',loadCaps);
$('sq').addEventListener('input',()=>{$('sqv').textContent=$('sq').value;put('sq',$('sq').value)});
$('pq').addEventListener('input',()=>$('pqv').textContent=$('pq').value);
let busy=false;
async function shutter(){
  if(busy)return; busy=true; $('shot').disabled=true; $('msg').className=''; $('msg').textContent='撮影中…';
  $('view').removeAttribute('src');   // プレビューを止めて、静止画に帯域を譲る
  try{
    const r=await fetch(`/api/shutter?board=${encodeURIComponent($('board').value.trim())}&q=${$('sq').value}`,{method:'POST'});
    const j=await r.json(); if(!r.ok)throw new Error(j.detail||r.statusText);
    $('msg').className='ok';
    $('msg').textContent=`保存: ${j.file}  ${j.width}×${j.height}  ${(j.bytes/1000).toFixed(0)} kB  ${j.seconds} 秒`;
    loadGallery();
  }catch(e){$('msg').className='bad';$('msg').textContent='失敗: '+e.message}
  busy=false; $('shot').disabled=false; startPreview();
}
$('shot').onclick=shutter;

// ---- 露出・ピントの操作 (ボードの /ctrl を camtool 経由で呼ぶ) ----
const bq=()=>'board='+encodeURIComponent($('board').value.trim());
async function api(method,path,params){
  const qs=new URLSearchParams(params||{}).toString();
  const r=await fetch(`${path}?${bq()}${qs?'&'+qs:''}`,{method});
  const j=await r.json().catch(()=>({}));
  if(!r.ok)throw new Error(j.detail||r.statusText);
  return j;
}
let caps=null, st=null, touched={}, afBusy=false;
const touch=k=>touched[k]=Date.now(), fresh=k=>Date.now()-(touched[k]||0)<1500;
// シャッターは対数目盛り: スライダ 0..1000 <-> min..max µs
const s2us=v=>Math.round(caps.exposure_us.min*Math.pow(caps.exposure_us.max/caps.exposure_us.min,v/1000));
const us2s=us=>Math.round(1000*Math.log(us/caps.exposure_us.min)/Math.log(caps.exposure_us.max/caps.exposure_us.min));
function fmtShutter(us){const x=1e6/us; return `1/${x>=10?Math.round(x):x.toFixed(1)} s (${us} µs)`}
function setSw(id,v){for(const b of $(id).children)b.classList.toggle('on',b.dataset.v===v)}
function cmsg(t,cls){$('cmsg').className=cls||'';$('cmsg').textContent=t}

async function loadCaps(){
  caps=null; $('ctl').classList.add('off');
  if(!$('board').value.trim())return;
  try{ caps=await api('GET','/api/ctrl/caps'); }
  catch(e){ cmsg('ボードが制御 API に対応していません (imx708_wifi_control を書き込んでください)','bad'); return }
  $('gain').min=caps.gain.min; $('gain').max=caps.gain.max;
  $('foc').min=caps.focus.min; $('foc').max=caps.focus.max;
  $('wbsec').style.display=caps.wb?'':'none'; $('wbbox').style.display=caps.wb?'block':'none';
  if(caps.wb){ for(const id of ['wbr','wbb']){$(id).min=caps.wb.min; $(id).max=caps.wb.max} }
  $('ctl').classList.remove('off'); cmsg('');
  poll();
}
function render(s){
  st=s;
  const em=s.exposure_mode==='manual', ga=s.gain_mode==='auto', fm=s.focus_mode==='manual';
  setSw('expsw',s.exposure_mode); setSw('focsw',s.focus_mode);
  $('gauto').checked=ga; $('gauto').disabled=!em;
  $('exp').disabled=!em; $('gain').disabled=!em||ga; $('foc').disabled=!fm;
  // 自分が今動かしているスライダは、ボードの値で上書きしない
  if(!fresh('exp'))$('exp').value=us2s(Math.max(s.exposure_us,caps.exposure_us.min));
  if(!fresh('gain'))$('gain').value=s.gain_index;
  if(!fresh('foc'))$('foc').value=s.focus_pos;
  $('expv').textContent=fmtShutter(fresh('exp')?s2us(+$('exp').value):s.exposure_us);
  const gi=fresh('gain')?+$('gain').value:s.gain_index;
  $('gainv').textContent='×'+(caps.gain.x[gi-caps.gain.min]??s.gain_x).toFixed(2);
  $('focv').textContent=fresh('foc')?$('foc').value:s.focus_pos;
  if(caps.wb){
    const wm=s.wb_mode==='manual', wbBusy=s.wb_state==='running';
    setSw('wbsw',s.wb_mode); $('wbr').disabled=$('wbb').disabled=!wm;
    if(!fresh('wbr'))$('wbr').value=s.wb_red; if(!fresh('wbb'))$('wbb').value=s.wb_blue;
    $('wbrv').textContent='×'+((fresh('wbr')?+$('wbr').value:s.wb_red)/1000).toFixed(2);
    $('wbbv').textContent='×'+((fresh('wbb')?+$('wbb').value:s.wb_blue)/1000).toFixed(2);
    $('wbbtn').disabled=wbBusy;
    $('wbv').textContent=wbBusy?'合わせています…':s.wb_state==='done'?'白を合わせました':s.wb_state==='failed'?'合わせられませんでした (枠に白いものを写して再試行)':'';
  }
  afBusy=s.af_state==='running';
  $('afbtn').disabled=afBusy; if(!busy)$('shot').disabled=afBusy;
  $('afv').textContent=afBusy?'合焦中…':s.af_state==='done'?`合焦 ${s.focus_pos} (${(s.af_ms/1000).toFixed(1)} s)`
    :s.af_state==='failed'?'AF 中断':'';
}
let pollTimer=null;
async function poll(){
  clearTimeout(pollTimer);
  if(!caps)return;
  try{ render(await api('GET','/api/ctrl')); if(/接続/.test($('cmsg').textContent))cmsg(''); }
  catch(e){ cmsg('ボードの状態を読めません: '+e.message,'bad') }
  pollTimer=setTimeout(poll,afBusy?300:1000);
}
// 送信は 150ms 以上空けて、最新の値だけを送る
let pending=null, inflight=false, lastSent=0, sendTimer=null;
function send(p){ pending=Object.assign(pending||{},p); pump() }
async function pump(){
  if(inflight||!pending)return;
  const wait=150-(Date.now()-lastSent);
  if(wait>0){ if(!sendTimer)sendTimer=setTimeout(()=>{sendTimer=null;pump()},wait); return }
  const p=pending; pending=null; inflight=true; lastSent=Date.now();
  try{ render(await api('POST','/api/ctrl',p)); }catch(e){ cmsg('設定できません: '+e.message,'bad') }
  inflight=false; pump();
}
// クリック後にフォーカスを外す: 残ると Space (シャッター) でもう一度押されてしまう
for(const [id,key] of [['expsw','exposure_mode'],['focsw','focus_mode'],['wbsw','wb_mode']])
  for(const b of $(id).children)b.onclick=()=>{b.blur(); send({[key]:b.dataset.v})};
$('gauto').onchange=()=>send({gain_mode:$('gauto').checked?'auto':'manual'});
function slider(id,fn){
  const h=()=>{touch(id); const p=fn(); if(st)render(st); send(p)};
  $(id).addEventListener('input',h); $(id).addEventListener('change',h);
}
slider('exp',()=>({exposure_us:s2us(+$('exp').value)}));
slider('gain',()=>({gain_index:+$('gain').value}));
slider('foc',()=>({focus_pos:+$('foc').value}));
slider('wbr',()=>({wb_red:+$('wbr').value}));
slider('wbb',()=>({wb_blue:+$('wbb').value}));
$('wbbtn').onclick=async()=>{
  $('wbbtn').blur();
  try{ await api('POST','/api/wb/onepush'); $('wbbtn').disabled=true; $('wbv').textContent='合わせています…'; setTimeout(poll,300) }
  catch(e){ cmsg('白を合わせられません: '+e.message,'bad') }
};
$('afbtn').onclick=async()=>{
  $('afbtn').blur();
  try{ await api('POST','/api/af'); afBusy=true; $('afbtn').disabled=true; $('afv').textContent='合焦中…'; poll() }
  catch(e){ cmsg('AF を開始できません: '+e.message,'bad') }
};
$('reset').onclick=async()=>{
  if(!confirm('保存された露出・ピント・ホワイトバランスの設定を消して、Auto に戻しますか?'))return;
  try{ render(await api('POST','/api/config/reset')); cmsg('設定をリセットしました','ok') }
  catch(e){ cmsg('リセットできません: '+e.message,'bad') }
};
document.addEventListener('keydown',e=>{if(e.code==='Space'&&!/INPUT|SELECT/.test(document.activeElement.tagName)){e.preventDefault();shutter()}});
async function loadGallery(){
  const j=await (await fetch('/api/captures')).json();
  $('gal').innerHTML=j.slice(0,12).map(f=>`<a href="/captures/${f}" target="_blank"><img loading="lazy" src="/captures/${f}"></a>`).join('');
}
loadGallery(); startPreview(); loadCaps();
</script></html>"""


def build_app(default_board: str):
    from fastapi import FastAPI, HTTPException, Request
    from fastapi.responses import HTMLResponse
    from fastapi.staticfiles import StaticFiles

    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    app = FastAPI(title="esp32p4-cam tool")

    @app.get("/", response_class=HTMLResponse)
    def index():
        return PAGE.replace("__BOARD__", default_board)

    @app.post("/api/shutter")
    def shutter(board: str, q: int = 95):
        try:
            return take_shot(board, q)
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise HTTPException(502, f"ボードに接続できません: {e}")
        except ValueError as e:
            raise HTTPException(400, str(e))

    def relay(board: str, method: str, path: str, params: dict | None = None):
        try:
            return board_call(board, method, path, params)
        except BoardError as e:
            raise HTTPException(e.status, f"ボード: {e.detail}")
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise HTTPException(502, f"ボードに接続できません: {e}")
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.get("/api/ctrl/caps")
    def ctrl_caps(board: str):
        return relay(board, "GET", "/ctrl/caps")

    @app.get("/api/ctrl")
    def ctrl_state(board: str):
        return relay(board, "GET", "/ctrl")

    @app.post("/api/ctrl")
    def ctrl_set(request: Request, board: str):
        params = {k: v for k, v in request.query_params.items() if k != "board"}
        bad = sorted(set(params) - set(CTRL_KEYS))
        if bad:
            raise HTTPException(400, f"使えないキー: {', '.join(bad)}")
        return relay(board, "POST", "/ctrl", params)

    @app.post("/api/af")
    def af(board: str):
        return relay(board, "POST", "/af")

    @app.post("/api/wb/onepush")
    def wb_onepush(board: str):
        return relay(board, "POST", "/wb/onepush")

    @app.post("/api/config/reset")
    def config_reset(board: str):
        return relay(board, "POST", "/config/reset")

    @app.get("/api/captures")
    def captures():
        return sorted((p.name for p in CAPTURE_DIR.glob("*.jpg")), reverse=True)

    app.mount("/captures", StaticFiles(directory=CAPTURE_DIR), name="captures")
    return app


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="ブラウザ UI を起動")
    s.add_argument("--board", default=os.getenv("CAM_BOARD", ""), help="ボードの IP / ホスト名")
    s.add_argument("--host", default="127.0.0.1")
    s.add_argument("--port", type=int, default=8080)
    t = sub.add_parser("shot", help="フルサイズ静止画を 1 枚撮って保存")
    t.add_argument("--board", default=os.getenv("CAM_BOARD", ""), required=not os.getenv("CAM_BOARD"))
    t.add_argument("-q", "--quality", type=int, default=95)
    t.add_argument("-o", "--out", type=Path, default=CAPTURE_DIR, help="保存先ディレクトリ")
    a = ap.parse_args()

    if a.cmd == "shot":
        try:
            r = take_shot(a.board, a.quality, a.out)
        except Exception as e:
            sys.exit(f"失敗: {e}")
        print(f"{a.out / r['file']}  {r['width']}x{r['height']}  {r['bytes']} bytes  {r['seconds']}s")
        return

    import uvicorn
    print(f"http://{a.host}:{a.port}  (ボード: {a.board or '未設定'})")
    uvicorn.run(build_app(a.board), host=a.host, port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
