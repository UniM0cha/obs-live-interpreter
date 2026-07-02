# Glossa (교회 실시간 설교 통역)

한국어 설교를 **실시간으로 외국어 음성으로 통역**하여, 외국인 성도가 무선 이어폰으로
자신의 언어로 들을 수 있게 하는 OBS Studio 플러그인입니다.

> *Glossa*(γλῶσσα)는 "혀·방언·언어"를 뜻하는 헬라어로, 오순절(사도행전 2장)에 나오는 단어입니다.
> *"우리가 다 우리의 각 언어(Glossa)로 하나님의 큰 일을 말함을 듣는도다"* — 사도행전 2:11

---

## 설치

플러그인 바이너리는 [GitHub Releases](https://github.com/UniM0cha/glossa/releases) 에서 OS 에 맞는 파일을 받습니다.
(네이티브 플러그인이라 macOS·Windows 빌드는 서로 호환되지 않습니다.)

**macOS (Apple Silicon 전용 — Intel 미지원)**
1. `*-macos-arm64.pkg` 를 받아 더블클릭 설치 (서명·공증되어 Gatekeeper 경고 없음).
2. OBS Studio(**31.x** 권장) 재시작.

**Windows (x64)**
1. `*-windows-x64-installer.exe` 실행. SmartScreen 경고가 뜨면 *추가 정보 → 실행*.
   설치 위치가 OBS 가 스캔하는 머신 폴더(`C:\ProgramData\obs-studio\plugins`)라 UAC(관리자) 프롬프트가 뜹니다 — *예* 를 누릅니다.
2. OBS Studio 재시작.

## 사용 — 번역 서버 연결 (필수)

플러그인은 음성을 번역하는 **서버**와 짝을 이룹니다. 서버를 먼저 띄운 뒤(→ [`server/README.md`](server/README.md)),
OBS 에서 **보기 → 도크 → Glossa** 로 통역 도크를 열어 설정합니다:

- **Server URL** — 폰 청취 페이지와 **같은 주소** `https://<호스트>` (로컬 테스트는 `http://localhost:8000`).
- **Service Key** — 서버의 `SERVICE_KEY` 와 동일하게.
- **번역 엔진** — Gemini 또는 OpenAI.
- **설교자 음색** — 주소를 입력하면 서버에 등록된 설교자가 드롭다운에 자동으로 채워집니다.
  "설교자 본인 목소리로 변환" 을 체크하면 선택한 설교자 음색으로 통역됩니다.

**통역할 오디오** 목록에서 소스를 **체크**(여러 개면 하나로 합성)하고 **통역 시작** 을 누르면,
도크에 청취자 수·진행 시간 등 서버 상태가 표시됩니다. 외국인 성도는 휴대폰 웹페이지에서
언어를 골라 번역 음성·실시간 자막을 듣습니다.

### (선택) 설교자 음색 변환
통역 음성을 **설교자 본인 목소리**로 바꿔 들려주는 기능입니다. 운영자가 서버에 설교자 음색
(ElevenLabs voice clone)을 등록하면 도크 드롭다운에 자동으로 나타납니다. 실명·`voice_id` 는
플러그인이 아니라 **서버 env(`SPEAKERS_JSON`)에만** 둡니다. 설정은 [`server/README.md`](server/README.md) 참고.

> 참고: 번역 엔진의 원본 음성은 화자 톤을 따라가는 방식이라 **침묵 후 음색이 바뀔 수 있습니다**
> (Gemini/OpenAI 공식 문서에 명시된 한계, 2026-07-02 확인 — 출처는 [`server/README.md`](server/README.md)
> 의 "음색·용어 한계" 절 참고). 음색을 고정하고 싶으면 이 음색 변환 기능을 사용하세요.

## 제거

- **macOS**: `rm -rf ~/Library/Application\ Support/obs-studio/plugins/glossa.plugin` 후 OBS 재시작.
  (`pkgutil --forget com.unim0cha.glossa` 로 설치 기록도 정리 가능.)
- **Windows**: **설정 > 앱 > glossa > 제거** (또는 설치 폴더의 `unins000.exe`).

---

## 더 알아보기

- **개발·빌드·릴리즈·IDE 설정** → [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)
- **아키텍처 불변 규칙·번역 API 검증 스펙·작업 지침** → [`CLAUDE.md`](CLAUDE.md)
- **서버(다국어 fan-out·배포)** → [`server/README.md`](server/README.md)
