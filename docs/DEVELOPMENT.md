# Glossa — 개발 / 메인테이너 가이드

사용자용 설치·사용법은 [`README.md`](../README.md), 아키텍처 불변 규칙·번역 API 검증 스펙·작업
지침은 [`CLAUDE.md`](../CLAUDE.md), 서버 상세는 [`server/README.md`](../server/README.md) 를 참고하세요.

## 어떻게 동작하나

- OBS 도크에서 체크한 오디오 소스의 출력을 `audio_capture_callback` 으로 **읽기 전용 tap** 한다(원본 불변).
- 여러 소스를 타임스탬프 정렬로 모노 믹싱 → 엔진 입력 레이트(Gemini 16kHz / OpenAI 24kHz)로 리샘플 →
  단일 WebSocket 으로 서버 `/ingress` 에 업링크. 리샘플·전송 등 무거운 작업은 전부 워커 스레드(콜백 비블로킹).
- 서버는 구독자 있는 언어만 번역 세션을 fan-out → 휴대폰 웹클라이언트로 번역 음성·실시간 자막을 스트리밍.
- 방송으로 나가는 원본(한국어)은 **절대 변형하지 않는다** — 통역은 옆길(side-channel)로만 흐른다.
  (상세 불변 규칙은 [`CLAUDE.md`](../CLAUDE.md).)

## 구현 현황

- **Phase 0 — 파이프라인 검증** (`prototype/`, Python): OBS 없이 지연·품질·비용 선검증. ✅
- **Phase 1 — OBS 플러그인** (`plugin/`, C++/libobs/Qt): tap 필터 + 워커 + WebSocket + 통역 도크. ✅
- **Phase 2 — 다국어/다수 청취자** (`server/`, FastAPI/Railway): 언어별 fan-out, 폰 청취·자막. ✅

## 번역 엔진

번역 엔진(Gemini / OpenAI)은 **서버에서 실행**되고, 플러그인은 도크 선택값을 `/ingress?...&engine=`
으로 넘긴다. 두 엔진 **출력은 모두 24kHz** 라 폰 재생 경로는 동일하고, 입력 레이트만 다르다(Gemini 16k / OpenAI 24k).
모델 ID·오디오 포맷·가격 등 검증된 스펙은 [`CLAUDE.md`](../CLAUDE.md) 의 "검증된 사실" 절을 단일 출처로 본다(중복 방지).

## 개발 환경 (server)

저장소 루트의 단일 venv(`.venv/`)를 쓴다(디렉토리별 venv 만들지 않음).

```bash
uv venv && uv pip install -r requirements.txt   # 루트 requirements.txt 가 server/ 매니페스트 포함
source .venv/bin/activate
cd server && uvicorn app:app --port 8000
```

> Railway 배포는 `server/` 를 빌드 컨텍스트로 `server/requirements.txt` 를 그대로 쓴다.

## IDE 인덱싱 (CLion / IntelliJ)

macOS 빌드는 Xcode generator 를 강제하므로(`plugin/cmake/macos/compilerconfig.cmake` 의 `if(NOT XCODE) FATAL_ERROR`)
IDE 인덱싱이 깨진다(Xcode 모델로 열면 `<vector>` 같은 표준 헤더조차 "Cannot find file"). **인덱싱 전용**으로 Ninja 를 쓴다:

1. **`plugin/` 디렉토리**를 프로젝트로 연다(CMakeLists 가 repo 루트가 아니라 `plugin/` 에 있음).
2. CMake 프로필: 제너레이터 = **Ninja**, 옵션 = **`-DALLOW_NON_XCODE_GENERATOR=ON`**.
3. Reload → OBS 의존성(`plugin/.deps`) 내려받은 뒤 `<vector>`·`<obs.h>`·`<QComboBox>` 까지 인덱싱된다.

> 탈출구는 기본 **OFF**(Xcode 강제 유지)이며 IDE 인덱싱 전용이다. Ninja 로는 서명 `.plugin` 패키징이
> 불완전하니 **실제 빌드·릴리즈는 반드시 Xcode generator**(`cmake --preset macos` / CI `macos-ci`)로 한다.

## 릴리즈 배포 (메인테이너)

GitHub Actions 가 **태그 push** 시 macOS `.pkg`·Windows `.exe` 를 빌드해 **draft** 릴리즈로 올린다.

```bash
# 1) plugin/buildspec.json 의 "version" 을 새 값으로 올려 커밋 (안 올리면 아티팩트가 옛 버전으로 빌드됨)
# 2) develop → main 머지·push (= Railway 배포 트리거)
git tag -a v1.2.0 -m "..." && git push origin v1.2.0   # 같은 태그 재릴리즈 불가 → 항상 새 번호
```

- macOS: `macos-15` 러너에서 arm64 `.pkg` + Developer ID 서명 + Apple 공증.
- Windows: `windows-2022` 러너에서 x64 빌드 + Inno Setup 단일 `installer.exe`.
- CI 가 만든 릴리즈는 **draft + 본문은 체크섬뿐**이다 → 변경점 노트를 채워 **Publish** 해야 공개된다.
  (`gh release edit <tag> --notes-file notes.md --draft=false --latest`. 노트 범위는 `git log <직전태그>..HEAD`.)
- macOS 서명·공증엔 저장소 Secrets 7종 필요(`MACOS_SIGNING_*` 5종, `MACOS_NOTARIZATION_*` 2종). 미설정 시 미서명 빌드.
- Actions 의 **Dispatch**(`workflow_dispatch`) 워크플로로 서명·릴리즈 없이 빌드만 검증 가능.

> 트리거 워크플로는 repo 루트 `.github/workflows/`, 빌드 스크립트·액션은 `plugin/.github/` 에 있다(`plugin/` 이 플러그인 루트).

## 디렉토리

```
glossa/
├── README.md          # 사용자용 설치·사용
├── CLAUDE.md          # 작업 지침 / 검증 규칙 / 아키텍처 불변 규칙
├── docs/DEVELOPMENT.md# 이 문서
├── plugin/            # Phase 1: C++ OBS 플러그인 (libobs/CMake/Qt)
└── server/            # Phase 2: 클라우드 동반 서버 (FastAPI, 다국어 fan-out)
```

## 참고 링크

- [Gemini Live Translate](https://ai.google.dev/gemini-api/docs/live-api/live-translate) ·
  [OpenAI Realtime Translation](https://developers.openai.com/api/docs/guides/realtime-translation)
- [OBS Plugins docs](https://docs.obsproject.com/plugins) ·
  [Source API Reference](https://docs.obsproject.com/reference-sources) ·
  [obs-async-audio-filter (비동기 오디오 필터 참고)](https://github.com/norihiro/obs-async-audio-filter)
