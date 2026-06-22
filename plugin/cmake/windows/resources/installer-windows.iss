; Inno Setup 스크립트 — Glossa (Windows x64, per-user 설치)
;
; 값(이름/버전/소스경로/출력)은 빌드 시 ISCC 의 /D 정의로 주입된다.
;   → plugin/.github/scripts/Package-Windows.ps1 참고
; macOS .pkg(installer-macos.pkgproj.in)와 대칭이며, 관리자 권한 없이
; 현재 사용자의 OBS 플러그인 폴더에 설치한다.

#ifndef MyAppName
  #define MyAppName "glossa"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef MyAppPublisher
  #define MyAppPublisher "Glossa"
#endif
#ifndef MySourceDir
  #define MySourceDir "."
#endif
#ifndef MyOutputDir
  #define MyOutputDir "."
#endif
#ifndef MyOutputBase
  #define MyOutputBase "glossa-installer"
#endif

[Setup]
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; per-user 설치 — 관리자 권한 불필요
PrivilegesRequired=lowest
DefaultDirName={userappdata}\obs-studio\plugins\{#MyAppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName}
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBase}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
WizardStyle=modern

[Files]
; obs-studio 플러그인 로드 레이아웃: <name>\bin\64bit\<name>.dll + <name>\data\
Source: "{#MySourceDir}\bin\64bit\*"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
