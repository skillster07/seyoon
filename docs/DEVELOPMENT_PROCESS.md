# VIVIDCAM 클라우드 우선·Windows 마일스톤 검증 프로세스

## 기본 원칙

VIVIDCAM은 클라우드에서 구현·정적 검증·자동 테스트를 최대한 완료한 뒤, 실제 장치가 필요한 지점마다 Windows 로컬 검증을 수행합니다. Windows 검증을 제품 마지막으로 미루지 않고 캡처, GPU, 가상 카메라, AI 효과, 플랫폼 연동 마일스톤마다 반복합니다.

## 매 반복 작업의 완료 순서

1. 클라우드에서 기능과 실패 경로를 구현합니다.
2. Linux와 Windows CI에서 컴파일하고 공통 단위 테스트를 실행합니다.
3. 구현 범위와 아직 검증되지 않은 범위를 문서에 표시합니다.
4. 커밋과 PR을 생성합니다.
5. 하드웨어 게이트가 있는 마일스톤은 로컬 Windows 검증 명령을 실행합니다.
6. 로그·성능 수치·오류 코드를 저장하고 다음 클라우드 반복에 입력합니다.

## 자동 CI 게이트

| 게이트 | 클라우드 검증 |
| --- | --- |
| Web | TypeScript 검사, Vite production build |
| Native Linux | C++20 경고 오류 처리 빌드, CTest, portable diagnostics |
| Native Windows | MSVC 빌드, CTest, Media Foundation·D3D11 diagnostics 초기화 |
| PR | Linux·Windows 자동 게이트가 모두 통과해야 병합 |

Windows CI는 Windows 전용 헤더와 라이브러리의 컴파일·링크 오류를 발견하지만 물리 카메라, 실제 GPU 성능, SOOP·TikTok·OBS 장치 인식까지 증명하지는 않습니다.

Native CTest 타깃은 Release 빌드에서도 `NDEBUG`를 해제하여 assertion 기반 검증이 실제로 실행되도록 구성합니다.

## 로컬 Windows 하드웨어 게이트

`native/scripts/validate-windows.ps1` 전체 검증은 보호된 HKLM producer identity를 읽고
필요하면 repair하므로 64-bit elevated PowerShell과 현재 validation 계정 SID를 요구합니다.
스크립트는 HKLM COM source 경로, 설치된 source DLL·diagnostics·engine과 현재 Release
build의 각 SHA-256, 정확한 5-value manifest와 현재 계정 `EngineUserSid`, protected owner·
3-ACE DACL이 모두 일치할 때만 설치를 current로 인정합니다. 하나라도 다르면 all-users
설치를 다시 실행하고 같은 계약을 재검증한 뒤 나머지 gate를 진행합니다. repair/reinstall이
필요할 때 설치 engine이 실행 중이면 교체가 거부되므로 먼저 종료해야 합니다. 이 자동
package 검증은 실제 설치 engine ↔ FrameServer handshake·heartbeat 통합 결과를 대신하지
않습니다.

### Gate W1 — 카메라 캡처

```powershell
.\native\build\Release\vividcam_diagnostics.exe --capture-test
```

통과 기준:

- 권장 포맷이 1920×1080 60 FPS 또는 장치의 최선 60 FPS로 선택됨
- 3초 동안 프레임을 1개 이상 수신
- `errors=0`
- 종료 후 다른 앱에서 카메라 재사용 가능

### Gate W2 — D3D11 GPU 표면

통과 기준:

- `[gpu] D3D11 Hardware` 출력
- 캡처 통계에서 `gpu > 0`
- 정상 지원 장치에서 `cpu=0`
- 1080p60 10분 테스트 중 device removed 오류 없음

### Gate W3 — 프리뷰·합성

통과 기준:

- 실제 카메라 프리뷰 60 FPS
- 레이어 이동·크기 조정·투명도 변경이 프레임을 멈추지 않음
- 렌더 p95 16.67ms 미만

### Gate W4a — 가상 카메라 등록·activation 계약

통과 기준:

- all-users COM 서버가 Frame Server에서 읽을 수 있는 위치에 설치됨
- `IMFActivate`와 필수 Media Source/Stream 인터페이스 계약 통과
- `MFCreateVirtualCamera` 등록·시작·정지·제거 수명주기 통과

### Gate W4b-0 — 영구 등록 소스 테스트 패턴

통과 기준:

- `System + CurrentUser` 카메라가 프로세스 종료·재부팅 이후에도 유지됨
- 반환된 symbolic link를 `MFCreateDeviceSource`로 열 수 있음
- 1920×1080 NV12 60p 계약의 서로 다른 테스트 패턴 샘플을 12개 이상 수신
- timestamp가 단조 증가하고 duration이 60p이며 프레임 내용이 변화함
- 제거 명령이 PnP 장치와 설정을 먼저 지운 뒤 COM 서버를 제거함

### Gate W4b-1 — 사용자 세션 엔진 호스트

통과 기준:

- 관리자 권한이나 카메라 초기화 없이 별도 `vividcam_engine`이 장시간 실행됨
- Created → Starting → Running → Stopping → Stopped 전환과 첫 종료 사유가 유지됨
- steady clock heartbeat 순번·uptime·누락 interval이 단조 증가함
- 제한 시간 smoke가 5초 안에 종료 코드 0으로 끝남
- Windows Ctrl+C와 portable SIGINT·SIGTERM 처리기는 플래그만 기록하고 엔진 루프가 정상 종료함
- Frame Server IPC 전에는 텔레메트리가 `frame_transport=unavailable`을 명시함

### Gate W4b-2a — versioned control IPC·producer identity binding

통과 기준:

- `VCIP` 1.0 고정 64-byte little-endian header와 안정 메시지 ID golden test 통과
- 잘못된 magic·major·type·길이·reserved·sequence·trailing bytes를 명확히 거부
- 단일 source CLSID stable route를 engine/source가 공유하고 SHA-256 pipe token만 노출
- canonical production pipe의 보호 DACL은 SYSTEM과 정확한
  `NT SERVICE\FrameServer` service SID만 허용
- server가 `SourceHello` 뒤 client를 impersonate하여 LocalService user와 enabled
  FrameServer service SID를 함께 확인하고 모든 경로에서 복귀
- `GetNamedPipeClientProcessId`가 SCM이 보고한 실행 중 FrameServer PID와 일치해야 함
- 비canonical 테스트 route만 SYSTEM·LocalService·현재 logon SID loopback 정책을 유지
- 설치된 엔진 pipe가 있는 동안 일반 사용자 `--control-client-test`는 예상 접근 거부를
  확인해 `[control-client-denial] win32=5 [valid]`와 종료 코드 0을 반환해야 함
- all-users 설치가 source DLL과 sibling `vividcam_engine.exe`를
  `C:\Program Files\VIVIDCAM\VirtualCamera`에 배치하고 해시를 재검증
- `HKLM\Software\VIVIDCAM\VirtualCamera\ProducerIdentity`에 정확한
  `SchemaVersion` DWORD 1, `Generation` QWORD, `EnginePath` SZ,
  elevated installer 계정의 `EngineUserSid` SZ, `EngineSha256` 32-byte BINARY 기록
- manifest key는 상속 없는 정확한 세 allow ACE만 가짐: SYSTEM·Administrators
  `KEY_ALL_ACCESS`, FrameServer service SID `KEY_QUERY_VALUE | READ_CONTROL`
- 설치 파일은 FrameServer 중지 전에 stage·hash하고, 중지 뒤 backup과 transactionally
  교체·재검증; manifest는 `Generation=0` in-progress 뒤 최종 generation을 마지막에 기록
- 설치 transaction 실패 시 이전 파일 해시와 manifest 값·형식·보안 descriptor를 정확히
  rollback·재검증하고, uninstall은 FrameServer를 COM 등록 삭제까지 계속 중지
- source가 Hello 전에 server PID와 일반 사용자 token을 검사하고 engine user SID가
  manifest SID와 같은지, token·pipe session이 현재 active console session과 같은지,
  token이 non-elevated·medium integrity 이하인지 확인
- process image, manifest path와 source DLL sibling path는 각각 regular non-reparse disk
  file이어야 하고 `GetFinalPathNameByHandle` 최종 경로까지 같아야 하며, 파일 SHA-256을
  상수 시간 비교
- source가 위 token identity·session·path·hash 전체를 매 producer heartbeat마다 재검증하고
  실패 시 ACK 없이 연결을 끊음
- engine은 기존 DACL을 보존하고 FrameServer service SID 직접 ACE에는 현재 process의
  `PROCESS_QUERY_LIMITED_INFORMATION`과 primary token의 `TOKEN_QUERY`만 추가하며,
  이 최소 권한 설정 실패 시 시작 거부
- 엔진 선·후기동, heartbeat stale, 서버 종료·재시작 뒤 자동 재연결 통과
- pending overlapped I/O를 취소하고 source Stop·Shutdown이 2초 안에 worker를 회수
- producer 부재·protocol 오류·재연결 중에도 `RequestSample` 테스트 패턴 경로가 비차단 유지
- identity binding은 VCIP 1.0 wire·message ID·payload를 바꾸지 않으며 HMAC secret을
  추가하지 않음
- manifest 형식·DACL mismatch의 fail-closed 처리 구현, 경로·SHA mismatch와 일반 사용자
  canonical client 거부 negative test 통과; 실제 SID·active console·elevation·integrity와
  다른 service principal은 설치 통합 gate에서 재확인
- unsigned 개발 빌드의 설치 경로·SHA-256 pin은 중간 gate이며, 배포 전 Authenticode
  signer SPKI pin과 필요 시 restricted broker/package 경계를 추가
- Program Files·HKLM을 변경하는 관리자는 신뢰하며, 같은 사용자 runtime injection·process
  hollowing은 현재 범위 밖. canonical pipe precreation availability DoS도 후속 완화 필요
- 현재 active console session 한 개만 지원하며 RDP-only·fast user switching·복수 동시
  session은 후속 broker·ACL-protected per-registration route 설계에서 지원
- 자동 게이트 뒤 관리자 재설치, 설치된 엔진의 일반 사용자 실행, 실제 FrameServer
  handshake·heartbeat를 확인해야 로컬 설치 통합 항목 완료

### Gate W4b — 방송 앱 가상 카메라 수신

통과 기준:

- SOOP, TikTok LIVE Studio, OBS 장치 목록에 표시
- 세 앱에서 1920×1080 60 FPS 수신
- 앱 종료·재시작·장치 재연결 정상

### Gate W5 — AI 뷰티·배경

통과 기준:

- 효과 활성 상태 1080p60
- 얼굴 추적 지연 50ms 이하
- 저조도·가림·빠른 움직임 테스트 통과
- 원본 얼굴 영상이 로컬 처리 경계를 벗어나지 않음

### Gate W6 — 장시간 상용 안정성

통과 기준:

- 4시간 연속 1080p60 방송
- 프레임 드롭 1% 미만
- 비정상 종료 후 프로젝트 복구율 99%
- 메모리·GPU 메모리 지속 증가 없음

## 검증 결과 전달 템플릿

```text
Commit:
Windows version:
CPU / GPU / RAM:
Camera / capture card:
Input selected format:
Output profile:
Target application: SOOP | TikTok LIVE Studio | OBS

received=
consumed=
overwritten=
gpu=
cpu=
errors=

Average FPS:
Render p50 / p95 / max:
CPU / GPU / VRAM peak:
Error code and full log:
```

## 현재 마일스톤

- 2026-08-26 로컬 완료: W1 최선 60 FPS 캡처, W2 GPU surface, W3 1080p60 오프스크린 합성·NV12 변환, W4a COM activation·등록 수명주기, W4b-0 등록 소스 1080p60 테스트 패턴 수신 항목, W4b-1 일반 사용자 엔진 host bounded·Ctrl+C 종료, 기본 W4b-2a Windows control loopback·재연결 및 설치 DLL LocalService handshake·heartbeat
- 검증 근거: `docs/validation/WINDOWS_W1_W4A_2026-08-26.md`, `docs/validation/WINDOWS_W4B0_2026-08-26.md`, `docs/validation/WINDOWS_W4B1_ENGINE_HOST_2026-08-26.md`, `docs/validation/WINDOWS_W4B2A_CONTROL_IPC_2026-08-26.md`, `docs/validation/WINDOWS_W4B2A_PRODUCER_IDENTITY_2026-08-26.md`
- 입력 한계: 현재 캡처보드 입력은 720×480 60 FPS이며 네이티브 1080p60 입력은 별도 검증 필요
- 현재 핵심 공백: W4b-2b CPU latest-frame bridge와 실제 합성 프레임 전달
- 로컬 후속: Windows 재부팅 뒤 W4b-0 영구 등록·재수신 확인
- 구현·자동 검증 완료: installer account SID·Program Files engine final path·SHA-256
  manifest, active console·non-elevated/medium token gate, heartbeat 재검증, FrameServer
  service SID·SCM PID production gate, transactional installer rollback, 최소 process/token
  ACE와 direct manifest/hash/path verifier test; Windows Release CTest 9/9, control
  transport 5회 반복, Web production build 통과
- 로컬 완료: 같은 active console 계정의 elevated 64-bit PowerShell에서
  `validate-windows.ps1`가 generation 1 package 설치·재검증과 등록 source 1920x1080 NV12
  60p 샘플 12개 수신을 통과. 이어 설치된 일반 사용자 엔진 ↔ 실제 FrameServer handshake
  1회, heartbeat ACK 147/147, protocol error 0, rejected peer 0 확인
- 클라우드 다음 범위: W4b-2b CPU latest-frame IPC → D3D11 공유 텍스처 IPC
- 로컬 다음 상태: OBS 등록 장치 컬러바·control 수신 통과 후 SOOP·TikTok LIVE Studio까지 1080p60 W4b 확장
- 병행 범위: D3D11 이미지·텍스트 렌더러, 데스크톱 UI bridge, 실제 1080p60 입력 및 장치 매트릭스
