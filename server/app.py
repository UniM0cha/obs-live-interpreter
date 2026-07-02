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
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

load_dotenv(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))  # server/.env (gitignore — 로컬 키 전부, cwd 무관)
from session_manager import SessionManager  # noqa: E402  (load_dotenv 먼저)
from voice import VoiceConverter, speaker_list  # noqa: E402

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
log = logging.getLogger("app")

SERVICE_KEY = os.environ.get("SERVICE_KEY", "changeme")
HERE = os.path.dirname(os.path.abspath(__file__))
ALLOWED_LANGS = {"en", "vi", "zh", "ja"}  # static/index.html 의 LANGS 와 동기 유지

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


async def drop_listener(lang: str, ws):
    """청취자 정리의 단일 경로 — listeners 집합과 SessionManager 구독자 카운트를
    정확히 1회씩 갱신한다. 전송 실패로 먼저 발견되든 receive 루프 종료(finally)로
    발견되든 중복 호출에 안전(집합 소속 여부로 판정)."""
    subs = listeners.get(lang)
    if subs is None or ws not in subs:
        return
    subs.discard(ws)
    if not subs:
        vc.clear_pending(lang)  # 마지막 청취자 이탈 — 버퍼 + 대기열 합성 작업 폐기(무청취 유료 합성 방지)
    # 카운트 감소를 close 보다 먼저 — 이 함수는 세션 태스크(broadcast) 안에서도 불리는데,
    # 세션 취소가 close 대기 중에 떨어지면 유령 구독자 카운트가 남기 때문.
    # (remove_subscriber 의 감소 자체는 진입 즉시 동기 반영됨)
    await mgr.remove_subscriber(lang)
    try:
        # 전송 실패로 발견된 소켓은 receive 루프가 아직 살아 있을 수 있다 — 명시적으로
        # 닫아 좀비(등록 해제됐지만 pong 은 받는) 연결을 클라이언트 재연결로 유도.
        # 취소로 못 닫히는 경우는 클라이언트 하트비트 타임아웃이 회수.
        await ws.close()
    except Exception:
        pass


async def broadcast_audio(lang: str, pcm: bytes):
    for ws in list(listeners.get(lang, ())):
        try:
            await ws.send_bytes(pcm)
        except Exception:
            await drop_listener(lang, ws)  # 전송 실패 = 죽은 소켓


async def broadcast_transcript(lang: str, text: str) -> int:
    """번역 transcript(델타)를 그 언어 청취자들에게 전송. 실제 전송 성공 수를 반환."""
    payload = json.dumps({"type": "transcript", "text": text})
    sent = 0
    for ws in list(listeners.get(lang, ())):
        try:
            await ws.send_text(payload)
            sent += 1
        except Exception:
            await drop_listener(lang, ws)
    return sent


# 음색 변환기: 합성 PCM 을 폰으로 송출(broadcast_audio 재사용)
vc = VoiceConverter(broadcast_audio)


async def gemini_audio_sink(lang: str, pcm: bytes):
    # 음색 변환 ON 이면 원본 번역음성은 버리고 목사님 음색 TTS 만 송출, OFF 면 원본 송출
    if not vc.enabled:
        await broadcast_audio(lang, pcm)


async def handle_transcript(lang: str, text: str):
    log.info("[%s] %s", lang, text)
    delivered = await broadcast_transcript(lang, text)
    if delivered:
        vc.feed(lang, text)   # 음색 ON 일 때만 내부에서 문장 단위 합성·송출
    else:
        # 실수신 청취자 0(구독 유예 중이거나 전부 죽은 소켓) — 들을 사람 없는 유료 TTS
        # 방지. 남은 문장 버퍼·대기열도 폐기해 재구독 시 낡은 음성이 재생되는 것을 막는다.
        vc.clear_pending(lang)


mgr = SessionManager(gemini_audio_sink, on_transcript=handle_transcript)


def status_payload() -> str:
    counts = {lang: len(s) for lang, s in listeners.items() if s}
    return json.dumps({
        "type": "status",
        "live": mgr.live,
        "engine": mgr.engine,
        "speaker": vc.speaker,
        "voice": vc.enabled,
        "langs": mgr.active_langs(),
        "durationSec": int(time.time() - live_since) if live_since else 0,
        "listeners": counts,        # 언어별 청취자 수
        "total": sum(counts.values()),
    })


async def broadcast_status():
    payload = status_payload()
    for lang, subs in list(listeners.items()):
        for ws in list(subs):
            try:
                await ws.send_text(payload)
            except Exception:
                await drop_listener(lang, ws)  # 죽은 소켓 발견 경로 일원화
    for ws in list(ingress_conns):  # 플러그인(ingress)에도 상태 전송
        try:
            await ws.send_text(payload)
        except Exception:
            pass  # ingress 정리는 ingress() 핸들러 finally 가 담당


@app.websocket("/ingress")
async def ingress(ws: WebSocket):
    if ws.query_params.get("key") != SERVICE_KEY:
        await ws.close(code=4401)
        log.warning("ingress 인증 실패")
        return
    global live_since
    engine = ws.query_params.get("engine", "gemini")
    speaker = ws.query_params.get("speaker")  # chae|lee|kwon → 음색 변환 ON, 없으면 OFF
    await ws.accept()
    ingress_conns.add(ws)
    live_since = time.time()
    vc.set_speaker(speaker)
    log.info("ingress 연결됨 → 서비스 LIVE (engine=%s, speaker=%s)", engine, speaker)
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
        for lang in list(vc.buffers):
            vc.flush(lang)   # 끊기기 전 남은 미완성 문장도 합성
        vc.set_speaker(None)
        vc.reset()
        log.info("ingress 끊김 → 서비스 IDLE")
        await mgr.set_live(False)
        await broadcast_status()


@app.websocket("/listen")
async def listen(ws: WebSocket):
    lang = ws.query_params.get("lang", "en")
    if lang not in ALLOWED_LANGS:
        # 무인증 엔드포인트 — 임의 lang 으로 업스트림 번역 세션이 생성되는 것을 차단
        await ws.close(code=4400)
        log.warning("listen 거부 — 미지원 lang=%r", lang[:20])
        return
    await ws.accept()
    listeners.setdefault(lang, set()).add(ws)
    await mgr.add_subscriber(lang)
    log.info("listen 연결됨 lang=%s (구독자 %d)", lang, len(listeners[lang]))
    try:
        await ws.send_text(status_payload())
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            # 폰 하트비트(ping→pong). 무인증 엔드포인트라 임의 텍스트를 파싱하지 않고
            # 클라이언트(static/index.html)가 보내는 고정 리터럴만 매칭한다.
            if msg.get("text") == '{"type":"ping"}':
                try:
                    await ws.send_text('{"type":"pong"}')
                except Exception:
                    break  # 전송 실패 = 죽은 소켓 → finally 정리로
    except WebSocketDisconnect:
        pass
    finally:
        await drop_listener(lang, ws)
        log.info("listen 끊김 lang=%s", lang)


@app.get("/speakers")
async def speakers(key: str = ""):
    """도크 설교자 드롭다운용 목록. service_key 로 보호(실명 노출 방지). voice_id 는 안 내려준다."""
    if key != SERVICE_KEY:
        return JSONResponse({"error": "unauthorized"}, status_code=401)
    return speaker_list()


@app.get("/")
async def index():
    return FileResponse(os.path.join(HERE, "static", "index.html"))


app.mount("/static", StaticFiles(directory=os.path.join(HERE, "static")), name="static")
