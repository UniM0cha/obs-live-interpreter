"""언어별 실시간 번역 세션 fan-out + 구독자 게이팅 (엔진 선택: Gemini / OpenAI).

핵심 규칙:
- 한 언어 = 번역 세션 1개. 같은 언어 청취자들은 이 세션 출력을 공유.
- 세션은 (서비스 LIVE) AND (그 언어 구독자 ≥ 1) 일 때만 ON → 낭비/비용 방지.
  단, 마지막 구독자가 끊기면 SUB_GRACE_SEC 동안 세션 연결만 유지하고 입력 feed 는
  중단(비용 0에 가깝게) — 폰 순간 재접속 시 teardown/재기동(수 초 공백)을 피한다.
- 공유 한국어 PCM 을 활성 세션 전부에 fan-out 입력.
- 엔진은 ingress(=OBS 플러그인)가 고른 값을 SessionManager.set_live(engine=) 로 받는다.
- 두 엔진 출력 모두 24kHz PCM16 mono → 폰 재생 경로 무변경.

검증 출처(2026-06-20, 1차):
- Gemini: https://ai.google.dev/gemini-api/docs/live-api/live-translate (입력 16k / 출력 24k)
- OpenAI: https://developers.openai.com/api/docs/guides/realtime-translation (입력/출력 24k)
"""
import asyncio
import base64
import json
import logging
import os
import time

from google import genai
from google.genai import types
import websockets

log = logging.getLogger("session")

# Gemini
GEMINI_MODEL = "gemini-3.5-live-translate-preview"
GEMINI_IN_RATE = 16000   # Gemini 입력: 16kHz mono 16-bit

# OpenAI
OPENAI_MODEL = "gpt-realtime-translate"
OPENAI_URL = f"wss://api.openai.com/v1/realtime/translations?model={OPENAI_MODEL}"
OPENAI_IN_RATE = 24000   # OpenAI 입력: 24kHz mono 16-bit (플러그인이 24k 로 보냄)

OUT_RATE = 24000  # 양 엔진 공통 출력: 24kHz mono 16-bit
IN_Q_MAX = 100    # 입력 큐 상한(≈10s @100ms). outage 시 무한 증가 방지 — 가득 차면 oldest 드롭.
SUB_GRACE_SEC = 5.0  # 구독자 0 유예 — 폰 순간 재접속 시 비싼 번역 세션 teardown/재기동 방지


class BaseLanguageSession:
    """엔진 무관 골격: 입력 큐 + start/stop/feed + 끊김 시 자동 재접속(_run).

    실제 업스트림 접속/스트리밍은 엔진별 _connect_and_stream() 에서. _run() 이 그걸
    감싸 끊기면 backoff 재접속하고, 재접속 시 밀린(stale) 오디오를 버려 라이브부터 재개한다.
    """

    ENGINE = "base"

    def __init__(self, lang, broadcast, on_transcript=None):
        self.lang = lang
        self.broadcast = broadcast          # async fn(lang, pcm_bytes)
        self.on_transcript = on_transcript  # optional async fn(lang, text)
        self.in_q: asyncio.Queue = asyncio.Queue(maxsize=IN_Q_MAX)
        self._task = None
        self._running = False

    def feed(self, pcm: bytes):
        if not self._running:
            return
        try:
            self.in_q.put_nowait(pcm)
        except asyncio.QueueFull:
            try:
                self.in_q.get_nowait()  # 가득 차면 oldest 드롭(라이브 우선)
            except asyncio.QueueEmpty:
                pass
            try:
                self.in_q.put_nowait(pcm)
            except asyncio.QueueFull:
                pass

    def _drain_queue(self):
        while not self.in_q.empty():
            try:
                self.in_q.get_nowait()
            except asyncio.QueueEmpty:
                break

    async def start(self):
        if self._running:
            return
        self._running = True
        self._task = asyncio.create_task(self._run())
        log.info("[%s] 세션 시작 (engine=%s)", self.lang, self.ENGINE)

    async def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        self._drain_queue()
        log.info("[%s] 세션 종료 (engine=%s)", self.lang, self.ENGINE)

    async def _run(self):
        """끊기면 backoff 재접속(라이브 유지). 실제 접속/스트리밍은 _connect_and_stream() 에서."""
        backoff = 1.0
        while self._running:
            self._drain_queue()  # 재접속 시 밀린(stale) 오디오 폐기 → 라이브부터 재개
            started = time.monotonic()
            try:
                await self._connect_and_stream()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                reason = repr(e)
            else:
                reason = "정상 종료"
            if not self._running:
                break
            if time.monotonic() - started >= 5:
                backoff = 1.0  # 충분히 가동 후 끊김 = 일시 장애 → backoff 리셋
            log.warning("[%s] %s 끊김(%s) → %.0fs 후 재접속", self.lang, self.ENGINE, reason, backoff)
            try:
                await asyncio.sleep(backoff)
            except asyncio.CancelledError:
                raise
            backoff = min(backoff * 2, 15.0)

    async def _connect_and_stream(self):
        raise NotImplementedError


class GeminiTranslateSession(BaseLanguageSession):
    """Gemini Live Translate 세션 (입력 16k / 출력 24k)."""

    ENGINE = "gemini"

    def __init__(self, lang, client, broadcast, on_transcript=None):
        super().__init__(lang, broadcast, on_transcript)
        self.client = client

    async def _connect_and_stream(self):
        config = types.LiveConnectConfig(
            response_modalities=["AUDIO"],
            output_audio_transcription=types.AudioTranscriptionConfig(),
            translation_config=types.TranslationConfig(
                target_language_code=self.lang,
                echo_target_language=False,
            ),
        )
        async with self.client.aio.live.connect(model=GEMINI_MODEL, config=config) as session:
            async def sender():
                while True:
                    pcm = await self.in_q.get()
                    await session.send_realtime_input(
                        audio=types.Blob(data=pcm, mime_type=f"audio/pcm;rate={GEMINI_IN_RATE}")
                    )

            async def receiver():
                async for response in session.receive():
                    sc = response.server_content
                    if not sc:
                        continue
                    if sc.output_transcription and sc.output_transcription.text and self.on_transcript:
                        await self.on_transcript(self.lang, sc.output_transcription.text)
                    if sc.model_turn:
                        for part in sc.model_turn.parts:
                            if part.inline_data and part.inline_data.data:
                                await self.broadcast(self.lang, part.inline_data.data)

            await asyncio.gather(sender(), receiver())


class OpenAITranslateSession(BaseLanguageSession):
    """OpenAI gpt-realtime-translate 세션 (입력/출력 24k).

    프로토콜(1차 출처):
      - 접속 후 session.update 로 출력 언어 지정.
      - 입력: session.input_audio_buffer.append (base64 24k pcm16). VAD 자동, commit 불필요.
      - 출력: session.output_audio.delta (base64) / session.output_transcript.delta (text).
    ⚠️ preview 모델 — 미처리 이벤트는 debug 로깅해 E2E 시 실제 타입을 확인/보정.
    """

    ENGINE = "openai"

    def __init__(self, lang, api_key, broadcast, on_transcript=None):
        super().__init__(lang, broadcast, on_transcript)
        self.api_key = api_key

    async def _connect_and_stream(self):
        headers = {"Authorization": f"Bearer {self.api_key}"}
        async with websockets.connect(OPENAI_URL, additional_headers=headers, max_size=None) as ws:
            # 출력 언어 설정 (입력은 24k pcm16 고정)
            await ws.send(json.dumps({
                "type": "session.update",
                "session": {"audio": {"output": {"language": self.lang}}},
            }))

            async def sender():
                while True:
                    pcm = await self.in_q.get()
                    await ws.send(json.dumps({
                        "type": "session.input_audio_buffer.append",
                        "audio": base64.b64encode(pcm).decode("ascii"),
                    }))

            async def receiver():
                async for raw in ws:
                    if isinstance(raw, bytes):  # 번역 오디오/전사는 JSON 텍스트로 옴
                        continue
                    try:
                        evt = json.loads(raw)
                    except (ValueError, TypeError):
                        continue
                    t = evt.get("type", "")
                    if t == "session.output_audio.delta":
                        d = evt.get("delta") or evt.get("audio")
                        if d:
                            await self.broadcast(self.lang, base64.b64decode(d))
                    elif t == "session.output_transcript.delta":
                        d = evt.get("delta") or evt.get("text")
                        if d and self.on_transcript:
                            await self.on_transcript(self.lang, d)
                    elif t == "error" or t.endswith(".error"):
                        log.warning("[%s] OpenAI 이벤트 오류: %s", self.lang, evt)
                    else:
                        log.debug("[%s] OpenAI 미처리 이벤트: %s", self.lang, t)

            await asyncio.gather(sender(), receiver())


class SessionManager:
    def __init__(self, broadcast, on_transcript=None):
        self.broadcast = broadcast
        self.on_transcript = on_transcript
        self.gemini_key = os.environ.get("GEMINI_API_KEY")
        self.openai_key = os.environ.get("OPENAI_API_KEY")
        self.gemini_client = genai.Client(api_key=self.gemini_key) if self.gemini_key else None
        if not self.gemini_key:
            log.warning("GEMINI_API_KEY 없음 — gemini 엔진 사용 불가")
        if not self.openai_key:
            log.warning("OPENAI_API_KEY 없음 — openai 엔진 사용 불가")
        self.engine = "gemini"
        self.sessions: dict[str, BaseLanguageSession] = {}
        self.subs: dict[str, int] = {}
        self.live = False
        self._lock = asyncio.Lock()
        self._grace: dict[str, asyncio.Task] = {}  # lang -> 유예 reconcile 태스크(참조 유지로 GC 방지)

    def feed_korean(self, pcm: bytes):
        for lang, s in self.sessions.items():
            # 구독자 0(유예 중) 세션은 연결만 유지하고 입력은 중단 — 유예 동안
            # 유료 입력 토큰이 나가지 않게 (비용 게이팅: LIVE AND 구독자>0)
            if self.subs.get(lang, 0) > 0:
                s.feed(pcm)

    @staticmethod
    def _normalize_engine(engine: str) -> str:
        return engine if engine in ("gemini", "openai") else "gemini"

    def _engine_key_ok(self) -> bool:
        return bool(self.openai_key if self.engine == "openai" else self.gemini_key)

    def _make_session(self, lang: str) -> BaseLanguageSession:
        if self.engine == "openai":
            return OpenAITranslateSession(lang, self.openai_key, self.broadcast, self.on_transcript)
        return GeminiTranslateSession(lang, self.gemini_client, self.broadcast, self.on_transcript)

    async def set_live(self, live: bool, engine: str = "gemini"):
        # 엔진 간 폴백 안 함: 입력 레이트(16k/24k)는 플러그인의 엔진 선택과 묶여 있어,
        # 키 없다고 다른 엔진으로 바꾸면 레이트 불일치로 번역 음성이 깨진다. 키 없으면 미시작.
        self.live = live
        if live:
            self.engine = self._normalize_engine(engine)
            if not self._engine_key_ok():
                log.error("선택 엔진 '%s' 의 API 키가 없습니다 — 세션 미시작. (.env 확인)", self.engine)
        else:
            # 서비스 종료 — 구독자 유예 무시하고 즉시 정리
            for t in self._grace.values():
                t.cancel()
            self._grace.clear()
        await self._reconcile()

    async def add_subscriber(self, lang: str):
        self.subs[lang] = self.subs.get(lang, 0) + 1
        grace = self._grace.pop(lang, None)
        if grace:
            grace.cancel()  # 유예 중 재구독 — 세션 유지
        await self._reconcile()

    async def remove_subscriber(self, lang: str):
        self.subs[lang] = max(0, self.subs.get(lang, 0) - 1)
        if self.subs.get(lang, 0) == 0 and lang in self.sessions:
            # 마지막 구독자가 끊김 — 재연결일 수 있으니 언어별 유예 후 재평가.
            # 타이머는 항상 최신 끊김 기준으로 갱신(플랩핑 시 조기 종료 방지).
            # 보호할 세션이 있을 때만 예약 — /listen 은 무인증이라 임의 lang 연결/해제로
            # 유휴 태스크가 쌓이는 것을 막는다.
            # 밀려 있던 입력 PCM 도 폐기 — 유예 중 무청취 업로드/낡은 번역 방지.
            self.sessions[lang]._drain_queue()
            old = self._grace.pop(lang, None)
            if old:
                old.cancel()
            self._grace[lang] = asyncio.create_task(self._reconcile_after_grace(lang))
        else:
            await self._reconcile()

    async def _reconcile_after_grace(self, lang: str):
        await asyncio.sleep(SUB_GRACE_SEC)
        self._grace.pop(lang, None)
        await self._reconcile()

    def active_langs(self):
        return sorted(self.sessions.keys())

    async def _reconcile(self):
        """각 언어 세션을 (live AND 구독자>0) 상태 + 현재 엔진에 맞춘다."""
        async with self._lock:
            langs = set(self.subs) | set(self.sessions)
            engine_ready = self._engine_key_ok()
            for lang in langs:
                should = self.live and self.subs.get(lang, 0) > 0 and engine_ready
                cur = self.sessions.get(lang)
                running = cur is not None
                engine_ok = running and cur.ENGINE == self.engine
                if (not should and running and lang in self._grace
                        and self.live and engine_ready and cur.ENGINE == self.engine):
                    # 구독자 0 "만"이 이유일 때만 유예 — live 종료·엔진 교체·키 소실은
                    # 유예 없이 즉시 정리해야 함(구 엔진 세션이 잘못된 레이트 PCM 을 받는 것 방지)
                    continue
                if should and not running:
                    s = self._make_session(lang)
                    self.sessions[lang] = s
                    await s.start()
                elif should and running and not engine_ok:
                    # 엔진이 바뀜 → 기존 세션 종료 후 새 엔진으로 교체
                    await cur.stop()
                    s = self._make_session(lang)
                    self.sessions[lang] = s
                    await s.start()
                elif not should and running:
                    s = self.sessions.pop(lang)
                    await s.stop()
