# 클라우드 동반 서버 (Python)

OBS 플러그인이 올린 한국어 16k PCM을 받아, **구독자 있는 언어만** Gemini Live Translate 세션을
돌려 번역 음성을 폰으로 스트리밍한다. (전체 설계: 상위 [README.md](../README.md))

## 구성
- `app.py` — FastAPI. `/ingress`(플러그인 WS, 서비스 키 인증) · `/listen?lang=`(폰 WS) · `/`(웹클라이언트)
- `session_manager.py` — 언어별 Gemini 세션 fan-out + 구독자 게이팅 (live AND 구독자>0 일 때만 세션 ON)
- `static/index.html` — 폰 웹클라이언트 (언어 선택 + Web Audio 재생 + LIVE/IDLE 상태)
- `fake_plugin.py` / `test_listener.py` — OBS 없이 서버를 검증하는 하니스

## 로컬 실행
```bash
cd server
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env        # GEMINI_API_KEY, SERVICE_KEY 채우기
uvicorn app:app --host 0.0.0.0 --port 8000
```
폰을 같은 WiFi에 두고 `http://<PC-LAN-IP>:8000` 접속(QR로 안내). 운영자 PC의 OBS 플러그인은
서버 URL(`ws(s)://.../ingress`)과 `SERVICE_KEY`로 접속.

## OBS 없이 검증 (M2.0/M2.2)
```bash
# 터미널 A: 서버
uvicorn app:app --port 8000
# 터미널 B: 가짜 폰(영어) — out_en.wav 저장
python test_listener.py en out_en.wav
# 터미널 C: 가짜 폰(베트남어)
python test_listener.py vi out_vi.wav
# 터미널 D: 가짜 플러그인 — 한국어 샘플 업로드 → 위 두 폰에 각 언어 음성 도착
python fake_plugin.py
```
검증값(2026-06-20): 한국어 샘플 → en/vi 동시 번역 정상, 첫 음성 ~4.6s.

## 클라우드 배포 (Railway 등 상시 운영)
서버는 항상 켜져 있고, OBS 플러그인이 접속해 "번역 ON" 하면 그때 예배가 LIVE 가 된다.
`Procfile` 이 `$PORT` 바인딩을 처리하므로 Railway/Render 등에서 바로 뜬다.

1. 이 저장소를 Railway 프로젝트로 연결, **Root Directory = `server`** 로 지정.
2. 환경변수 설정(대시보드): `GEMINI_API_KEY`, `SERVICE_KEY`. (`.env` 는 배포 안 됨 — gitignore)
3. 배포되면 공개 URL(`https://<app>.up.railway.app`) 발급.
   - 폰/QR: `https://<app>.up.railway.app`
   - OBS 플러그인: 서버 URL `wss://<app>.up.railway.app`, 서비스 키 = `SERVICE_KEY`
4. Railway 가 `requirements.txt` + `Procfile` 을 자동 인식(nixpacks). Python 3.11+ 에서 동작.

> WebSocket(`/ingress`, `/listen`)은 Railway 프록시가 그대로 지원. TLS 는 Railway 가 종단 →
> 폰은 `wss`, 플러그인도 `wss` 로 접속(IXWebSocket SecureTransport).

## 엔드포인트
| 경로 | 용도 | 인증 |
|---|---|---|
| `WS /ingress?key=` | 플러그인 PCM 업로드(16k s16 mono). 접속=LIVE | `SERVICE_KEY` |
| `WS /listen?lang=` | 폰: 상태(JSON text)+번역음성(24k PCM binary) | 없음(후속: 룸코드) |
| `GET /` | 폰 웹클라이언트 | 없음 |

## env
- `GEMINI_API_KEY` — Gemini API 키 (커밋 금지, `.env`는 gitignore)
- `SERVICE_KEY` — 플러그인↔서버 공유 비밀
