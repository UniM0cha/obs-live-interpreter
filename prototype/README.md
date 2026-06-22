# Phase 0 — 파이프라인 검증 프로토타입

OBS C++ 플러그인에 투자하기 전에 **핵심 리스크(지연·품질·비용)** 를 먼저 제거하는 단계입니다.
자세한 배경은 상위 [`README.md`](../README.md) · [`CLAUDE.md`](../CLAUDE.md) 참고.

## 마일스톤 진행 순서

| 단계 | 내용 | 상태 |
|---|---|---|
| **A** | 파일 기반 API 검증 (오디오 장치 없음) — `milestone_a.py` | ✅ 구현됨 |
| **B** | 실시간 마이크 → 통역 음성 라이브 재생 — `milestone_b.py` | ✅ 구현됨 |
| C | BlackHole 라우팅 + 지연·비용 측정 | ⏳ 예정 |

> 실시간 오디오 I/O 복잡도 없이 "API가 쓸만한 번역 음성을 돌려주는가"부터 깬다 → A 가 1순위.

---

## 마일스톤 A 실행법

```bash
cd prototype

# 1) 가상환경 + 의존성
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

# 2) API 키 설정 (커밋 금지)
cp .env.example .env
#   .env 를 열어 GEMINI_API_KEY 채우기 — 발급: https://aistudio.google.com/apikey

# 3) 한국어 설교 샘플 생성 (macOS 내장 say + afconvert, 외부 의존성 없음)
./make_sample.sh
#   → samples/sermon_ko_16k.wav (16kHz mono 16-bit)
#   ※ 한국어 음성 미설치 시: 시스템 설정 > 손쉬운 사용 > 음성 콘텐츠 > 시스템 음성에 한국어(Yuna 등) 추가

# 4) 번역 검증 (기본: 한국어 → 영어)
python milestone_a.py
#   다른 언어: python milestone_a.py -l vi
#   직접 입력: python milestone_a.py -i 내파일.wav -o 결과.wav

# 5) 결과 듣기
afplay out_en.wav
```

### 무엇을 확인하나
- **품질**: 번역 음성이 자연스러운지, 억양/속도가 보존되는지 (귀로 확인)
- **지연**: 콘솔의 `첫응답지연` (송신 시작 → 첫 번역 음성 수신까지)
- **정확도**: `원문 ko` / `번역 en` 전사 텍스트 출력

### 안 될 때
- `GEMINI_API_KEY 가 없습니다` → `.env` 확인 또는 `export GEMINI_API_KEY=...`
- `TranslationConfig` 관련 AttributeError → `pip install -U google-genai` (preview API라 최신 SDK 필요)
- `번역 음성을 한 바이트도 받지 못했습니다` → 모델 ID가 바뀌었을 수 있음.
  [공식 문서](https://ai.google.dev/gemini-api/docs/live-api/live-translate)로 모델 ID·포맷 재확인 후
  `milestone_a.py` 상단 `MODEL` 상수와 상위 `CLAUDE.md`·`README.md` 갱신.

---

## 마일스톤 B 실행법 (실시간 마이크 → 통역)

마이크로 한국어를 말하면 실시간으로 번역 음성이 출력 장치로 재생됩니다.
**반드시 이어폰/헤드폰 사용** — 스피커로 출력하면 번역 음성이 다시 마이크로 들어가
피드백 루프가 생깁니다(상위 `README.md` 불변규칙 #2).

```bash
cd prototype
source .venv/bin/activate          # 마일스톤 A 에서 만든 가상환경 재사용

# (선택) 오디오 장치 목록 확인 / 마이크 입력 레벨 점검
python milestone_b.py --list-devices
python milestone_b.py --check-mic

# 실시간 통역 시작 (기본 한국어 → 영어, Ctrl+C 로 종료)
python milestone_b.py
#   다른 언어: python milestone_b.py -l vi
#   장치 지정: python milestone_b.py --in-dev 2 --out-dev 3
```

---

## 파일

- `milestone_a.py` — 파일 기반 번역 검증 스크립트 (verbatim 검증된 API 형태만 사용)
- `milestone_b.py` — 실시간 마이크 입력 → 통역 음성 라이브 재생 (sounddevice)
- `make_sample.sh` — 한국어 테스트 WAV 생성 (macOS say + afconvert)
- `requirements.txt` — 의존성 (A·B 필수 / C 주석)
- `.env.example` — 키 템플릿 (`.env` 로 복사해 사용, 커밋 금지)

생성물(`samples/`, `out*.wav`)은 `.gitignore` 처리되어 커밋되지 않습니다.
