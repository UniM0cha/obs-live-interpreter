"""Glossa — 클라우드 동반 서버.

- /ingress (WS): OBS 플러그인이 서비스 키로 접속해 16k 한국어 PCM 업로드. 접속=서비스 LIVE.
- /listen  (WS): 폰이 ?lang=xx 로 접속, 상태(JSON text) + 번역 음성(24k PCM binary) 수신.
- /        : 폰 웹클라이언트(static/index.html).

서비스 LIVE 이고 그 언어 구독자가 있을 때만 해당 Gemini 세션이 돈다(비용 게이팅).
"""
import asyncio
import json
import logging
import os
import time
from contextlib import asynccontextmanager

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

async def _status_ticker():
    """LIVE 동안 모니터링 상태(진행시간 등)를 주기적으로 푸시 — 플러그인 도크 갱신용."""
    while True:
        await asyncio.sleep(3)
        if live_since is not None:
            try:
                await broadcast_status()
            except Exception:
                pass


@asynccontextmanager
async def lifespan(_app):
    task = asyncio.create_task(_status_ticker())
    try:
        yield
    finally:
        task.cancel()


app = FastAPI(title="Glossa", lifespan=lifespan)

# lang -> set[WebSocket] (폰 구독자) / ingress 연결들 / LIVE 시작 시각(epoch)
listeners: dict[str, set] = {}
ingress_conns: set = set()
live_since: float | None = None


async def broadcast_audio(lang: str, pcm: bytes):
    for ws in list(listeners.get(lang, ())):
        try:
            await ws.send_bytes(pcm)
        except Exception:
            pass


async def broadcast_transcript(lang: str, text: str):
    """번역 transcript(델타)를 그 언어 청취자들에게 자막용 텍스트 메시지로 전송."""
    payload = json.dumps({"type": "transcript", "text": text})
    for ws in list(listeners.get(lang, ())):
        try:
            await ws.send_text(payload)
        except Exception:
            pass


async def handle_transcript(lang: str, text: str):
    log.info("[%s] %s", lang, text)
    await broadcast_transcript(lang, text)


mgr = SessionManager(broadcast_audio, on_transcript=handle_transcript)


def status_payload() -> str:
    counts = {lang: len(s) for lang, s in listeners.items() if s}
    return json.dumps({
        "type": "status",
        "live": mgr.live,
        "engine": mgr.engine,
        "langs": mgr.active_langs(),
        "durationSec": int(time.time() - live_since) if live_since else 0,
        "listeners": counts,        # 언어별 청취자 수
        "total": sum(counts.values()),
    })


async def broadcast_status():
    payload = status_payload()
    targets = [ws for subs in listeners.values() for ws in list(subs)]
    targets += list(ingress_conns)  # 폰 + 플러그인(ingress) 양쪽에 상태 전송
    for ws in targets:
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
    global live_since
    engine = ws.query_params.get("engine", "gemini")
    await ws.accept()
    ingress_conns.add(ws)
    live_since = time.time()
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
        ingress_conns.discard(ws)
        if not ingress_conns:
            live_since = None
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
