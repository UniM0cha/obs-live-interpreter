"""OBS Live Interpreter — 클라우드 동반 서버.

- /ingress (WS): OBS 플러그인이 서비스 키로 접속해 16k 한국어 PCM 업로드. 접속=서비스 LIVE.
- /listen  (WS): 폰이 ?lang=xx 로 접속, 상태(JSON text) + 번역 음성(24k PCM binary) 수신.
- /        : 폰 웹클라이언트(static/index.html).

서비스 LIVE 이고 그 언어 구독자가 있을 때만 해당 Gemini 세션이 돈다(비용 게이팅).
"""
import json
import logging
import os

from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

load_dotenv()  # .env (공유 기본값)
# .env.local: 머신별 실제 키(gitignore). .env 값을 오버라이드.
load_dotenv(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env.local"), override=True)
from session_manager import SessionManager  # noqa: E402  (load_dotenv 먼저)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
log = logging.getLogger("app")

SERVICE_KEY = os.environ.get("SERVICE_KEY", "changeme")
HERE = os.path.dirname(os.path.abspath(__file__))

app = FastAPI(title="OBS Live Interpreter")

# lang -> set[WebSocket]  (폰 구독자)
listeners: dict[str, set] = {}


async def broadcast_audio(lang: str, pcm: bytes):
    for ws in list(listeners.get(lang, ())):
        try:
            await ws.send_bytes(pcm)
        except Exception:
            pass


async def log_transcript(lang: str, text: str):
    log.info("[%s] %s", lang, text)


mgr = SessionManager(broadcast_audio, on_transcript=log_transcript)


def status_payload() -> str:
    return json.dumps({
        "type": "status",
        "live": mgr.live,
        "engine": mgr.engine,
        "langs": mgr.active_langs(),
    })


async def broadcast_status():
    payload = status_payload()
    for subs in listeners.values():
        for ws in list(subs):
            try:
                await ws.send_text(payload)
            except Exception:
                pass


@app.websocket("/ingress")
async def ingress(ws: WebSocket):
    if ws.query_params.get("key") != SERVICE_KEY:
        await ws.close(code=4401)
        log.warning("ingress 인증 실패")
        return
    engine = ws.query_params.get("engine", "gemini")
    await ws.accept()
    log.info("ingress 연결됨 → 서비스 LIVE (engine=%s)", engine)
    await mgr.set_live(True, engine=engine)
    await broadcast_status()
    try:
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            data = msg.get("bytes")
            if data:
                mgr.feed_korean(data)
    except WebSocketDisconnect:
        pass
    finally:
        log.info("ingress 끊김 → 서비스 IDLE")
        await mgr.set_live(False)
        await broadcast_status()


@app.websocket("/listen")
async def listen(ws: WebSocket):
    lang = ws.query_params.get("lang", "en")
    await ws.accept()
    listeners.setdefault(lang, set()).add(ws)
    await mgr.add_subscriber(lang)
    log.info("listen 연결됨 lang=%s (구독자 %d)", lang, len(listeners[lang]))
    try:
        await ws.send_text(status_payload())
        while True:
            msg = await ws.receive()  # 폰→서버 메시지는 무시(연결 유지/끊김 감지용)
            if msg["type"] == "websocket.disconnect":
                break
    except WebSocketDisconnect:
        pass
    finally:
        listeners.get(lang, set()).discard(ws)
        await mgr.remove_subscriber(lang)
        log.info("listen 끊김 lang=%s", lang)


@app.get("/")
async def index():
    return FileResponse(os.path.join(HERE, "static", "index.html"))


app.mount("/static", StaticFiles(directory=os.path.join(HERE, "static")), name="static")
