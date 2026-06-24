# Glossa (교회 실시간 설교 통역)

한국어 설교를 **실시간으로 외국어 음성으로 통역**하여, 외국인 성도가 무선 이어폰으로
자신의 언어로 들을 수 있게 하는 OBS Studio 플러그인 프로젝트입니다.

> *Glossa*(γλῶσσα)는 "혀·방언·언어"를 뜻하는 헬라어로, 오순절(사도행전 2장)에 나오는 단어입니다.
> *"우리가 다 우리의 각 언어(Glossa)로 하나님의 큰 일을 말함을 듣는도다"* — 사도행전 2:11

---

## 설치하기 (다른 PC 의 OBS 에 설치)

플러그인 바이너리는 [GitHub Releases](https://github.com/UniM0cha/glossa/releases) 에서 받습니다.
컴파일된 네이티브 플러그인이라 **플랫폼별 빌드가 다릅니다** (macOS 번들과 Windows DLL 은 서로 호환되지 않음).
자신의 OS 에 맞는 파일을 받으세요.

### macOS (Apple Silicon)
1. Releases 에서 `*-macos-arm64.pkg` 를 받습니다. (**Apple Silicon 전용** — Intel Mac 미지원)
2. `.pkg` 를 더블클릭해 설치합니다. (Developer ID 서명·공증되어 Gatekeeper 경고 없이 설치)
3. OBS Studio 를 재시작합니다. (OBS **31.x** 권장 — libobs ABI 호환)

### Windows (x64)
1. Releases 에서 `*-windows-x64-installer.exe` 를 받아 실행합니다.
2. 서명되지 않은 설치 파일이라 SmartScreen 경고가 뜨면 **추가 정보 → 실행**.
   현재 사용자 폴더(`%APPDATA%\obs-studio\plugins\`)에 설치되어 관리자 권한이 필요 없습니다.
3. OBS Studio 를 재시작합니다.

### 설치 후 — 번역 서버 연결 (필수)
이 플러그인은 **음성을 번역하는 서버**와 짝을 이룹니다. 서버 주소는 플러그인에 **고정돼 있지 않으니**,
서버를 직접 띄운 뒤(저장소의 `server/` 를 Railway 등에 배포 — 가이드는 [`server/README.md`](server/README.md))
OBS 에서 직접 입력하세요:

1. OBS 메뉴 **보기 → 도크 → Glossa** 로 통역 도크를 엽니다.
2. 도크에서 설정:
   - **Server URL**: 폰 청취 페이지와 **같은 주소** `https://<호스트>` 를 그대로 입력하세요
     (로컬 테스트면 `http://localhost:8000`). 플러그인이 내부에서 `wss://…/ingress` 로 변환합니다.
   - **Service Key**: 서버의 `SERVICE_KEY` 와 동일하게
   - **번역 엔진**: Gemini 또는 OpenAI
   - **설교자 음색**: 서버에 등록된 설교자가 드롭다운에 **자동으로 채워집니다**(주소 입력 후).
     "설교자 본인 목소리로 변환" 을 체크하면 선택한 설교자 음색으로 통역됩니다.
     서버에 설교자가 없으면 드롭다운에 안내 문구가 표시되고 음색 변환은 비활성입니다.
3. **통역할 오디오** 목록에서 통역에 넣을 소스를 **체크**합니다(여러 개 체크 시 하나로 합성됨).
4. **통역 시작** 을 누르면 통역이 시작되고, 도크에 청취자 수·진행 시간 등 서버 상태가 표시됩니다.

> 예전 버전의 **소스별 "Live Interpreter" 필터** 는 더 이상 쓰지 않습니다. 기존에 추가해 둔 필터가
> 있으면 소스 → 필터에서 삭제하고, 위 도크에서 체크하는 방식으로 사용하세요.

### (선택) 설교자 음색 변환 — "목소리 카피"
통역 음성을 **설교자 본인 목소리**로 바꿔 들려주는 기능입니다. 운영자가 서버에 설교자 음색을 등록하면,
플러그인 도크의 설교자 드롭다운에 자동으로 나타납니다. (실명·`voice_id` 는 코드/플러그인 바이너리에
박히지 않고 **서버 env 에만** 둡니다 — 다른 교회가 같은 플러그인을 써도 이름이 노출되지 않습니다.)

1. **설교자 음성 클론(ElevenLabs IVC)** — [ElevenLabs](https://elevenlabs.io) 에서 설교자의 깨끗한
   설교 음성(수 분 분량)으로 *Instant Voice Clone* 을 만들어 `voice_id` 를 얻습니다. (IVC 는 유료 플랜 필요.)
2. **서버 env 등록** (Railway 대시보드 → Variables, 또는 로컬 `server/.env`):
   - `ELEVENLABS_API_KEY` — ElevenLabs API 키
   - `SPEAKERS_JSON` — 설교자 목록(한 줄 JSON):
     `{"hong":{"label":"홍길동 목사","voice_id":"<위에서 얻은 voice_id>"}}`
     (`key`=내부 식별자 · `label`=도크 표시명 · `voice_id`=ElevenLabs IVC)
3. **OBS 도크에서 사용** — 서버 주소를 입력하면 설교자 드롭다운이 자동으로 채워집니다. 원하는 설교자를
   고르고 **"설교자 본인 목소리로 변환"** 을 체크하면, 통역 음성이 그 음색으로 합성되어 송출됩니다.

> 설교자 추가·변경은 서버 `SPEAKERS_JSON` 만 수정하면 됩니다(플러그인 재빌드 불필요).
> 미설정이면 드롭다운에 안내 문구가 뜨고 음색 변환은 비활성입니다. 서버 설정은 [`server/README.md`](server/README.md) 참고.

### 제거 (삭제)

- **macOS**: `.pkg` 는 언인스톨러를 만들지 않으니 플러그인 번들을 직접 지웁니다 (번들 하나뿐이라 단순):
  ```bash
  rm -rf ~/Library/Application\ Support/obs-studio/plugins/glossa.plugin
  pkgutil --forget com.unim0cha.glossa   # (선택) 설치 기록 정리
  ```
  Finder 라면 `~/Library/Application Support/obs-studio/plugins/` 에서 `glossa.plugin` 을 휴지통으로. 이후 OBS 재시작.
- **Windows**: **설정 > 앱 > glossa > 제거** (Inno Setup 이 만든 언인스톨러). 또는 설치 폴더의 `unins000.exe` 실행.

> 아키텍처별 차이나 직접 빌드(다른 Mac/Windows)에 대한 배경은 `CLAUDE.md` 와 아래 로드맵을 참고하세요.

---

## 1. 무엇을 만들려는가

- 예배 중 송출(OBS)되는 **설교 음성(한국어)** 을 가로채(tap) 복사합니다.
- 복사한 음성을 **Google Gemini Live API의 실시간 음성→음성 번역**에 흘려보냅니다.
- 번역되어 돌아온 **외국어 음성**을 OBS 송출 PC에 연결된 **이어폰(무선/유선)** 으로 재생합니다.
- 방송으로 나가는 원본(한국어)은 **절대 건드리지 않습니다.** 통역은 옆길(side-channel)로만 흐릅니다.

```
[설교 마이크/믹서]
       │ (48kHz PCM, OBS 내부)
       ▼
┌──────────────────────────────────────────────┐
│ OBS 오디오 소스                                │
│   └─ [Live Interpreter 필터]                   │
│         ├─ 원본 그대로 통과 ───────────────► OBS 송출/녹화 (한국어 유지)
│         └─ 복사본 tap                          │
└─────────────────┼────────────────────────────┘
                  │ 48kHz → 16kHz mono 리샘플, 100ms 청크
                  ▼
        [Gemini Live Translate API]  (WebSocket, audio-in / audio-out)
                  │ 24kHz mono PCM (번역된 외국어 음성)
                  ▼
        [출력 장치 = 이어폰]  (CoreAudio / miniaudio)
```

---

## 2. 핵심 기술 사실 (1차 출처 검증, 2026-06-20 기준)

> ⚠️ Gemini 번역 모델은 **preview** 상태입니다. 모델 ID·요금·포맷은 바뀔 수 있으니
> **구현 직전에 반드시 공식 문서로 다시 확인**하세요. (자세한 검증 규칙은 `CLAUDE.md` 참고)

### Gemini Live API — 실시간 번역
- **모델 ID**: `gemini-3.5-live-translate-preview` (preview)
- **동작 방식**: STT→번역→TTS로 쪼개지 않고, **오디오 입력 → 오디오 출력**을 한 모델이 end-to-end로 처리. 억양·속도·피치를 보존.
- **입력 오디오**: Raw 16-bit PCM, **16kHz**, mono, little-endian (권장 청크 ~100ms)
- **출력 오디오**: Raw 16-bit PCM, **24kHz**, mono, little-endian
- **언어 지정**: BCP-47 코드. `targetLanguageCode`(예: `"en"`, `"vi"`, `"zh"`)로 목표 언어 지정.
  `echoTargetLanguage`(boolean)로 "이미 목표 언어인 입력"을 따라 말할지/침묵할지 제어.
- **제약**: 이 번역 모드는 **tools / system instruction 미지원** — 순수 음성 번역만.
- **지원 언어**: 70개 이상 자동 감지.
- **접속 방식**:
  - SDK: Python `google-genai`(`from google import genai`), JS `@google/genai`
  - 또는 WebSocket 직접:
    `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent`
- **키 보안**: 클라이언트 배포 시 **ephemeral token**(현재 `v1alpha`) 권장. API 키 직접 노출 금지.

### OpenAI Realtime — 실시간 번역 (선택 가능한 대체 엔진)
> 번역 엔진은 **OBS 플러그인 메뉴에서 Gemini / OpenAI 중 선택**합니다. 엔진은 서버에서 돌고,
> 플러그인은 선택값을 `/ingress?...&engine=` 으로 넘깁니다. 두 엔진 **출력은 모두 24kHz로 동일**해
> 폰 재생 경로는 바뀌지 않습니다.
- **모델 ID**: `gpt-realtime-translate` (preview, 2026-05 출시)
- **입력/출력 오디오**: Raw 16-bit PCM, **24kHz**, mono, little-endian (입력은 base64 인코딩 송신)
- **언어 지정**: 접속 후 `session.update` → `session.audio.output.language`. 입력 70+ / **출력 13개**
  (en/es/pt/fr/ja/ru/zh/de/ko/hi/id/vi/it — 태국어·크메르어 없음).
- **접속**: `wss://api.openai.com/v1/realtime/translations?model=gpt-realtime-translate`,
  헤더 `Authorization: Bearer $OPENAI_API_KEY`. 입력 `session.input_audio_buffer.append`,
  출력 `session.output_audio.delta`.
- **가격**: 오디오 시간당 정액 **$0.034/분** (Gemini ≈$0.037/분과 동급).
- **출처**: https://developers.openai.com/api/docs/guides/realtime-translation

### OBS 플러그인 — 오디오 필터
- 필터는 `OBS_SOURCE_TYPE_FILTER` 타입의 source이며, 부모 source에 붙어 그 출력을 가로챕니다.
- 콜백: `obs_source_info.filter_audio(void *data, struct obs_audio_data *audio)`
  - 받은 `obs_audio_data`(PCM 샘플)를 **그대로 반환**하여 방송 음성은 변형하지 않습니다.
  - 반환 직전에 샘플을 **복사**해 옆길 큐로 넘깁니다 (tap).
  - 무거운 작업(네트워크/리샘플)은 필터 콜백 안에서 하지 말고 **별도 워커 스레드**에서 처리(오디오 콜백 블로킹 금지).
- 참고 구현: 비동기 오디오 필터 패턴 `norihiro/obs-async-audio-filter`.

---

## 3. 아키텍처 불변 규칙 (반드시 지킬 것)

1. **원본 불변**: `filter_audio`는 입력 PCM을 변형/치환하지 않고 그대로 통과시킨다. 통역 음성이 OBS 믹스/녹화/송출로 새어 들어가면 안 된다.
2. **피드백 차단**: 통역 출력(이어폰)이 다시 설교 마이크/OBS 입력으로 들어가 루프를 만들지 않게 한다. 출력은 전용 장치로만.
3. **오디오 콜백 비블로킹**: 리샘플·base64·WebSocket 전송·재생은 워커 스레드에서. 필터 콜백은 복사 후 즉시 반환.
4. **포맷 변환 명시**: OBS 내부(보통 48kHz float) → Gemini 입력(16kHz 16-bit mono) 리샘플. 출력은 24kHz로 받아 장치 샘플레이트에 맞춤.
5. **키는 커밋 금지**: API 키/토큰은 환경변수·설정 파일·OS 키체인으로. 절대 소스에 하드코딩하지 않는다.

---

## 4. 단계별 로드맵 (구현 현황)

### Phase 0 — 파이프라인 검증 (OBS 없이) — ✅ 구현됨 · `prototype/`
OBS C++에 투자하기 전에 **핵심 가설(지연시간·품질·비용)** 부터 증명한 단계.
- macOS에 가상 오디오 장치(예: **BlackHole**) 설치 → 시스템/설교 음성을 그쪽으로 라우팅.
- 작은 **Python 프로토타입**: `sounddevice`로 캡처 → `google-genai`로 번역 스트림 → 이어폰으로 재생.
- 측정: 체감 지연, 통역 자연스러움, 1~2시간 예배 기준 **API 비용**, 네트워크 끊김 시 동작.

### Phase 1 — 실제 OBS 플러그인 — ✅ 구현됨 · `plugin/`
- C++ + libobs, CMake(OBS 플러그인 템플릿) 기반.
- tap 오디오 필터 + 워커 스레드 + WebSocket 업링크 + 통역 도크(Qt) UI.
- 설정: 서버 URL·서비스 키, 번역 엔진(Gemini/OpenAI) 선택, 통역할 오디오 소스 체크(멀티소스 합성).

### Phase 2 — 다국어 / 다수 청취자 — ✅ 구현됨 · `server/`
한 명/한 언어 한계를 넘어, 여러 외국인이 각자 다른 언어로 듣도록 분배합니다.
- 클라우드 동반 서버(FastAPI, Railway 배포): 플러그인이 한국어 PCM을 업링크하면
  **구독자 있는 언어만** Gemini/OpenAI 세션을 fan-out 해 번역 음성을 폰으로 스트리밍.
- 성도는 휴대폰 웹클라이언트로 접속해 언어를 골라 번역 음성·실시간 자막 수신(Web Audio 재생).
- 세션 자동 재접속, 구독자 게이팅(비용 가드레일), 운영자용 도크 모니터링.

---

## 개발 환경 — Python (server)

`server/` 로컬 실행은 저장소 루트의 venv 하나(`.venv/`)를 씁니다.

```bash
# 저장소 루트에서 최초 1회
uv venv                              # 또는: python3 -m venv .venv
uv pip install -r requirements.txt   # 또는: source .venv/bin/activate && pip install -r requirements.txt
#   루트 requirements.txt 가 server/ 매니페스트를 포함합니다.
```

```bash
source .venv/bin/activate            # 저장소 루트에서 한 번 활성화
cd server && uvicorn app:app --port 8000
```

> Railway 배포는 `server/` 를 빌드 컨텍스트로 `server/requirements.txt` 를 그대로 사용합니다.

---

## 로컬 개발 — CLion / IntelliJ 인덱싱

이 플러그인은 OBS 플러그인 템플릿 기반이라 **macOS 빌드는 Xcode generator를 강제**합니다
([`plugin/cmake/macos/compilerconfig.cmake`](plugin/cmake/macos/compilerconfig.cmake) 의 `if(NOT XCODE) FATAL_ERROR`).
**실제 빌드·릴리스는 반드시 Xcode generator**로 하며, CI도 `cmake --preset macos-ci`(Xcode generator)로 빌드합니다
(번들 Info.plist·codesign·hardened runtime이 전부 `CMAKE_XCODE_ATTRIBUTE_*` 로만 적용되므로).

문제는 CLion / IntelliJ(C/C++ 플러그인) 인덱싱입니다:
- **Xcode generator로 열면** 프로젝트 모델은 로드되지만 CLion이 시스템 헤더 경로(libc++ `c++/v1`·SDK sysroot)를
  온전히 못 받아 `#include <vector>` 같은 **표준 헤더조차 "Cannot find file"** 로 뜨는 quirk가 있습니다.
- 그렇다고 **기본 Ninja/Make로 열면** 위 `if(NOT XCODE)` 가드에서 configure가 막힙니다.

**해결 — 인덱싱은 Ninja로 (탈출구 2개는 이미 `compilerconfig.cmake` 에 들어 있음):**
1. **`plugin/` 디렉토리를 프로젝트로 엽니다** (`CMakeLists.txt` 가 repo 루트가 아니라 `plugin/` 에 있음).
2. Settings → 빌드, 실행, 배포 → **CMake** → 프로필의 **제너레이터를 `Ninja`**, **CMake 옵션에 `-DALLOW_NON_XCODE_GENERATOR=ON`**.
3. CMake Reload → OBS 의존성(`plugin/.deps`)을 받은 뒤, Ninja가 파일별 컴파일 플래그(`-isysroot`·libc++ 포함)를
   그대로 넘겨 `<vector>`·`<obs.h>`·`<QComboBox>` 까지 모두 인덱싱됩니다.

> `compilerconfig.cmake` 의 탈출구는 ① `ALLOW_NON_XCODE_GENERATOR`(기본 **OFF** — Xcode 강제 유지)와
> ② 비-Xcode generator에서 비어 있는 `XCODE_VERSION` 검사를 건너뛰는 가드입니다. **둘 다 IDE 인덱싱 전용**이고
> 기본값에선 동작이 바뀌지 않아 빌드·릴리스(Xcode generator)에는 영향이 없습니다.
> Ninja로는 컴파일은 되지만 서명 `.plugin` 패키징은 불완전하니, **배포 빌드는 Xcode generator**로 하세요.

---

## 릴리스 배포 (메인테이너용)

GitHub Actions 가 **태그 push** 시 자동으로 macOS·Windows 설치 파일을 빌드해 Releases(draft)에 올립니다.

```bash
git tag v0.1.0
git push origin v0.1.0
```

- **macOS**: `macos-15` 러너에서 arm64 `.pkg` 빌드 + Developer ID 서명 + Apple 공증(notarization).
- **Windows**: `windows-2022` 러너에서 x64 `.dll` 빌드 + Inno Setup 단일 `installer.exe`.
  → Windows 컴퓨터나 Wine 없이, GitHub Actions 의 Windows 러너가 실제 MSVC 로 빌드합니다.
- 공개 저장소라 GitHub Actions(macOS 포함) 빌드 비용은 **무료**입니다.
- 산출물은 **draft** 릴리스로 생성되니, GitHub UI 에서 확인 후 **Publish** 하세요.
- macOS 서명/공증에는 저장소 Secrets 7종이 필요합니다
  (`MACOS_SIGNING_APPLICATION_IDENTITY`, `MACOS_SIGNING_INSTALLER_IDENTITY`, `MACOS_SIGNING_CERT`,
  `MACOS_SIGNING_CERT_PASSWORD`, `MACOS_KEYCHAIN_PASSWORD`, `MACOS_NOTARIZATION_USERNAME`,
  `MACOS_NOTARIZATION_PASSWORD`). 미설정 시에도 빌드는 되지만 미서명 산출물이 나옵니다.
- Actions 탭의 **Dispatch** 워크플로(`workflow_dispatch`)로 서명·릴리스 없이 빌드만 먼저 검증할 수 있습니다.

> CI 구성 파일: 트리거는 리포 루트 `.github/workflows/`, 빌드 스크립트·액션은 `plugin/.github/` 에 있습니다
> (`plugin/` 이 플러그인 루트이기 때문).

---

## 5. 알아둘 제약 / 리스크

- **지연시간**: "sub-second"라 해도 음성→음성 통역은 구문 단위로 약간의 지연이 생깁니다. 실시간 동시통역 수준의 기대치는 사전에 맞춰두세요.
- **비용**: Live API는 오디오 시간 기준 과금. 긴 예배에서 누적될 수 있으니 설교 구간에만 켜고 비용을 모니터링하세요.
- **네트워크 의존**: 예배 중 안정적 인터넷 필수. 끊김 시 재접속·무음 처리 로직 필요.
- **preview 모델**: 모델 ID/스펙이 예고 없이 바뀔 수 있음 — `CLAUDE.md`의 검증 규칙 준수.
- **프라이버시**: 설교 음성이 Google 클라우드로 전송됨. 필요하면 교회 측에 고지/동의를 고려.

---

## 6. 디렉토리 구조

```
glossa/
├── README.md          # 이 문서
├── CLAUDE.md          # Claude Code 작업 지침 / 검증 규칙
├── .gitignore
├── .github/           # 릴리스·빌드 트리거 GitHub Actions 워크플로
├── plugin/            # Phase 1: C++ OBS 플러그인 (libobs / CMake)
└── server/            # Phase 2: 클라우드 동반 서버 (FastAPI, 다국어 fan-out)
```

---

## 7. 참고 링크

**Gemini Live API / 번역**
- [Live translation with Gemini Live API — Google AI for Developers](https://ai.google.dev/gemini-api/docs/live-api/live-translate)
- [Live API capabilities guide](https://ai.google.dev/gemini-api/docs/live-api/capabilities)
- [Gemini Live API overview](https://ai.google.dev/gemini-api/docs/live-api)
- [Fluid, natural voice translation with Gemini 3.5 Live Translate (Google blog)](https://blog.google/innovation-and-ai/models-and-research/gemini-models/gemini-live-3-5-translate/)
- [Building a Real-Time Audio Translator with Gemini Live API (Kaz Sato, Google Cloud)](https://medium.com/google-cloud/building-a-real-time-audio-translator-with-gemini-live-api-03fb881b1774)

**OBS 플러그인**
- [OBS Plugins docs](https://docs.obsproject.com/plugins)
- [Source API Reference (obs_source_t)](https://docs.obsproject.com/reference-sources)
- [obs-async-audio-filter (비동기 오디오 필터 참고 구현)](https://github.com/norihiro/obs-async-audio-filter)
