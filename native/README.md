# VIVIDCAM Windows Native Spike

브라우저 UX와 별개로 장치의 최선 60p 입력을 1080p60으로 합성·출력하는 카메라 파이프라인을 구축하기 위한 C++20 기술 스파이크입니다.

## 현재 포함 범위

- SOOP·TikTok LIVE·OBS 60p 출력 프로필 및 유효성 검사
- 일정 지연 시 누락 프레임을 계측하는 60 FPS 프레임 스케줄러
- Windows Media Foundation 카메라 장치 및 네이티브 해상도·FPS·픽셀 포맷 열거
- 1080p60과 NV12를 우선하는 캡처 포맷 선택 정책
- D3D11 하드웨어/WARP 장치와 Media Foundation DXGI Device Manager
- GPU DXGI surface 우선·CPU buffer 폴백 비동기 캡처
- D3D11 Video Processor 기반 NV12/YUY2→BGRA 오프스크린 합성
- 가로·세로 회전, 재사용 가능한 출력 텍스처 풀, p50/p95/max 렌더 지연
- SOOP 가로·TikTok 세로·방송 대기 장면 템플릿과 레이어 렌더 플랜
- 레이어 순서·잠금·표시·변환 유효성 검사
- 장면 배경색과 카메라 위치·크기·투명도를 적용하는 D3D11 렌더 플랜 연결
- 프리뷰·가상 카메라·인코더·NDI 소비자별 latest-frame 출력 허브
- 가상 카메라용 60p Media Foundation 타임스탬프, 최신 프레임 교체, 입력 지연 시 마지막 프레임 반복
- Media Source용 bounded RequestSample 토큰 큐, start/stop/shutdown, flush와 discontinuity 상태 처리
- SOOP/TikTok 60p NV12 우선·BGRA 폴백 미디어 타입, stride 및 고정 샘플 크기 협상
- BT.709 limited-range BGRA→NV12 기준 변환과 2×2 평균 chroma subsampling
- D3D11 Video Processor BGRA→NV12 zero-copy 변환, 공유 NV12 텍스처 풀과 p50/p95/max 지연
- NV12/BGRA IMFMediaType 생성과 D3D11 NV12 텍스처의 IMFDXGIBuffer·IMFSample zero-copy 포장
- IMFMediaEventQueue 생성, stream started/stopped·GPU sample·error 이벤트 전달, nonblocking 회수와 shutdown
- NV12/BGRA IMFMediaType 목록을 가진 IMFStreamDescriptor, NV12 기본 타입과 선택된 IMFPresentationDescriptor
- 실제 IMFMediaSource/IMFMediaStream COM 객체, live source 특성, Start/Stop/Shutdown, bounded RequestSample과 GPU MEMediaSample 전달
- MFCreateVirtualCamera 세션/시스템 수명·현재/전체 사용자 접근, Start/Stop/Remove와 명시적 `--register-test`
- System+CurrentUser 영구 카메라 설치·제거와 등록 symbolic link 조회
- 등록 소스 전용 1920×1080 NV12/BGRA 60p 이동 컬러바와 실제 Frame Server consumer smoke gate
- 일반 사용자 세션에서 장시간 실행되는 `vividcam_engine`, 결정적 생명주기·heartbeat·종료 텔레메트리
- 64-byte little-endian versioned control protocol, strict decoder와 안정 메시지 ID
- 설치된 producer image에 결합된 Windows named-pipe control, cross-process heartbeat·재연결·취소 가능한 종료
- `Program Files` 엔진 경로·SHA-256·설치 사용자 SID를 고정하는 보호된 HKLM producer identity manifest
- RGBA 이미지·텍스트 스타일 리소스 저장소와 GPU 결과 비교용 색상/이미지 CPU 참조 합성기
- 플랫폼 진단 CLI
- 플랫폼 독립 코어 단위 테스트

현재 Windows 구현은 **1080p60 포맷 협상, 비동기 프레임 수신, D3D11·DXGI surface 전달, 단일 카메라 합성, W4a 등록, W4b-0 테스트 패턴, W4b-1 엔진 호스트와 W4b-2a control IPC·producer identity binding 단계**입니다. 2026-08-26 로컬에서 W1 최선 60 FPS 입력, W2 GPU surface, W3 1080p60 오프스크린 합성·NV12 변환과 W4a COM activation·등록 수명주기가 통과했습니다. W4b-0 영구 등록 장치도 실제 Frame Server consumer에서 1920×1080 NV12 60p 이동 컬러바 12개를 전달했습니다. W4b-1 엔진은 일반 사용자 권한에서 생명주기, heartbeat, 제한 시간과 Ctrl+C 종료가 통과했습니다. 기본 W4b-2a는 Windows loopback과 설치 DLL의 실제 Frame Server LocalService handshake 1회·heartbeat ACK 69/69를 오류 없이 통과했습니다. 그 뒤 추가한 producer identity binding은 Windows Release 빌드, **Windows CTest 8/8**, control transport 5회 반복 및 Web production build를 통과했습니다. 새 Windows 전용 `vividcam_install_registry_helpers`는 첫 설치의 상위 레지스트리 계층 생성·롤백과 Windows PowerShell 5.1 레지스트리 ACL 경로 호환성을 격리된 HKCU에서 검증합니다. elevated `validate-windows.ps1` package 판정·필요 시 자동 repair와 새 manifest의 실제 Frame Server 통합 게이트는 아직 대기입니다. 다음 구현은 CPU latest-frame producer bridge이며 W4b-0 재부팅 지속성도 아직 남아 있습니다.

## Linux/macOS 공통 코어 검증

```bash
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/vividcam_diagnostics
./native/build/vividcam_engine --run-for-ms 250 --heartbeat-ms 50
```

## Windows 빌드

Visual Studio 2022가 설치된 환경에서 실행합니다. 전체 검증 스크립트는 보호된 machine
producer identity까지 확인·복구하므로 **같은 active console 계정의 elevated 64-bit
PowerShell**에서 실행해야 합니다. 32-bit 또는 일반 사용자 PowerShell에서는 시작 전에
실패합니다. 설치 상태가 오래되어 repair/reinstall이 필요할 수 있으면 설치된
`vividcam_engine.exe`를 먼저 종료합니다.

```powershell
.\native\scripts\validate-windows.ps1

# 또는 빌드·프로세스 내부 검증만 단계별 실행(보호된 설치 상태 판정 제외)
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64
cmake --build native/build --config Release
ctest --test-dir native/build -C Release --output-on-failure
.\native\build\Release\vividcam_diagnostics.exe
.\native\build\Release\vividcam_diagnostics.exe --capture-test
.\native\build\Release\vividcam_diagnostics.exe --render-test
.\native\build\Release\vividcam_engine.exe --run-for-ms 250 --heartbeat-ms 50
```

검증 스크립트는 다음 조건을 모두 만족할 때만 all-users 설치를 현재 Release build와 같은
상태로 인정합니다.

- HKLM CLSID `InprocServer32` 기본 경로가 설치된 source DLL을 정확히 가리킴
- 설치된 source DLL·diagnostics·engine의 SHA-256이 현재 Release build와 각각 일치함
- producer manifest에 다른 값 없이 정확한 다섯 값이 있고 형식·값·길이가 유효함
- `EnginePath`가 설치 engine을 가리키고 `EngineUserSid`가 현재 elevated validation 계정
  SID와 같으며 `EngineSha256`이 설치 engine 해시와 일치함
- manifest owner·protected DACL과 SYSTEM·Administrators·FrameServer의 정확한 세 ACE가
  정책과 일치함

전체 스크립트는 먼저 Release build와 Windows CTest를 실행한 뒤 이 설치 상태를 판정합니다.
하나라도 다르면 all-users 패키지를 자동으로 repair/reinstall하고 같은 검사를 다시
수행합니다. 이때 설치된 engine이 실행 중이면 교체가 거부되므로 engine을 종료하고 다시
실행해야 합니다. package 재검증이 통과하면 실제 CLSID activation, W4a 수명주기와 W4b-0
영구 등록 장치 컬러바 수신까지 계속 검사합니다. Frame Server가 읽을 수 있도록
DLL·진단 프로그램·`vividcam_engine.exe`는
`C:\Program Files\VIVIDCAM\VirtualCamera`에 함께 설치합니다. 수동 설치와 제거는 관리자
PowerShell에서 다음 명령으로 수행할 수 있습니다.

```powershell
.\native\scripts\install-virtual-camera.ps1 -BuildDirectory .\native\build -SkipBuild -AllUsers
.\native\build\Release\vividcam_diagnostics.exe --registered-source-test
.\native\scripts\uninstall-virtual-camera.ps1 -BuildDirectory .\native\build -AllUsers
```

all-users 설치기는 설치된 엔진의 절대 경로·SHA-256과 현재 elevated installer 계정의
SID를 `HKLM\Software\VIVIDCAM\VirtualCamera\ProducerIdentity`에 기록합니다. manifest는
`SchemaVersion`(DWORD), `Generation`(QWORD), `EnginePath`(SZ),
`EngineUserSid`(SZ), `EngineSha256`(32-byte BINARY)의 다섯 값으로 구성됩니다. 상속을 끈
DACL에는 SYSTEM·Administrators FullControl과 `NT SERVICE\FrameServer`의
QueryValues·ReadPermissions만 둡니다.

설치 파일은 FrameServer를 멈추기 전에 같은 디렉터리의 임시 파일로 staging하고 SHA-256을
검증합니다. 서비스를 멈춘 뒤 기존 파일을 backup으로 옮기고 staged 파일을 교체하며,
manifest는 먼저 `Generation=0`으로 불완전 상태를 표시한 뒤 다른 네 값을 쓰고 최종
generation을 마지막에 기록합니다. 중간 실패 시 파일과 manifest 값·보안 descriptor를
이전 상태 그대로 복원하고 복원 해시까지 확인합니다. 실행 중인 설치 엔진을 교체해야 하면
설치는 명확히 실패합니다. all-users 제거는 FrameServer를 멈춘 상태에서 manifest·설치
파일뿐 아니라 COM 등록까지 삭제한 다음 서비스를 복구합니다.

Windows 진단 프로그램은 Media Foundation을 초기화하고 연결된 비디오 캡처 장치, 지원 포맷 및 VIVIDCAM이 선택한 우선 포맷을 출력합니다. `--capture-test`는 첫 번째 카메라를 선택 포맷으로 3초간 비동기 캡처하여 수신·소비·덮어쓴 프레임, GPU/CPU 경로와 오류 수를 보고합니다. `--render-test`는 GPU 프레임을 BGRA 렌더 타깃으로 합성하고 p50/p95/max 지연과 16.67ms W3 게이트를 검사합니다.

`--render-test`의 Media Foundation sample 전달은 진단 프로세스 안에서 생성한 source를
대상으로 합니다. `--registered-source-test`는 영구 카메라의 symbolic link를 통해
`MFCreateDeviceSource`로 다시 열어 Frame Server가 전달한 12개 NV12 컬러바 샘플을
검증합니다. OBS·SOOP·TikTok LIVE Studio에 엔진의 실제 합성 프레임을 전달하려면
W4b producer bridge와 별도 호환성 검증이 계속 필요합니다.

`vividcam_engine`은 Windows service나 관리자 프로세스가 아니라 설치를 실행한 계정과
같은 SID의 활성 콘솔 사용자 세션에서 실행합니다. production 연결은 build 디렉터리의
복사본이 아니라 설치된 다음 실행 파일을 일반 사용자 PowerShell에서 실행해야 합니다.
관리자 PowerShell이나 높은 integrity process에서 실행한 엔진은 거부됩니다.

```powershell
$engine = Join-Path $env:ProgramFiles "VIVIDCAM\VirtualCamera\vividcam_engine.exe"
& $engine
```

인자 없이 실행하면 Ctrl+C를 받을 때까지 계속 실행하고,
`--run-for-ms`는 CI와 smoke 검증을 위한 제한 실행입니다. 각 상태 행의
`[engine-control]` 행은 W4b-2a control worker의 연결·handshake·heartbeat 카운터입니다.
`frame_transport=unavailable`은 control 연결과 별개로 실제 영상 프레임 transport가 아직
연결되지 않았다는 뜻입니다.

canonical production route는 SYSTEM과 `NT SERVICE\FrameServer` SID만 pipe에
접속시킵니다. 따라서 예전에 사용한 일반 사용자 진단 명령은 더 이상 production 성공
handshake 검사가 아니라 예상된 접근 거부를 확인하는 negative test입니다. 설치된 엔진이
canonical pipe를 열고 있는 동안 실행합니다.

```powershell
.\native\build\Release\vividcam_diagnostics.exe --control-client-test
```

성공 출력은 다음과 같으며 이때 명령 자체는 종료 코드 0을 반환합니다.

```text
[control-client-denial] win32=5 [valid]
```

pipe가 없다는 결과는 성공이 아닙니다. 일반 사용자 진단 client의 access-denied 결과는
production DACL의 negative gate입니다.
새 production 성공 검증은 관리자 재설치 뒤 설치된 엔진을 일반 사용자로 실행하고,
등록 카메라를 열어 실제 Frame Server가 handshake·heartbeat를 수행하는 방식으로
진행합니다. 현재 그 새 설치 통합 결과는 대기 중이며
`docs/validation/WINDOWS_W4B2A_PRODUCER_IDENTITY_2026-08-26.md`에 상태를 기록합니다.
테스트용 비canonical route는 loopback CTest를 위해 기존 current-user 정책을 유지합니다.

VCIP wire format은 1.0 그대로이며 producer identity를 위한 secret이나 HMAC payload를
추가하지 않았습니다. 현재 바이너리가 서명되지 않아 설치기가 고정한 경로·SHA-256을
중간 신뢰 기준으로 사용합니다. 이 방식은 사용자 쓰기 가능 복사본과 단순 이름 위장을
막습니다. source는 manifest user SID, 활성 콘솔 session, non-elevated·medium 이하 token,
경로와 SHA-256을 handshake 전과 매 heartbeat마다 재검증합니다. 세 경로는 handle로 연
regular non-reparse file이어야 하며 `GetFinalPathNameByHandle` 결과도 같아야 합니다.

현재 신뢰 경계는 Program Files와 HKLM을 변경할 수 있는 관리자까지 포함합니다. 같은 사용자
권한의 runtime code injection·process hollowing은 범위 밖이고, 예측 가능한 canonical pipe를
먼저 만드는 availability DoS도 남아 있습니다. 현재는 물리적 활성 콘솔 세션 하나만 지원하며
RDP·복수 동시 세션은 후속 범위입니다. 배포 서명 이후 Authenticode 검증과 signer SPKI pin,
필요하면 restricted broker/package 경계를 추가합니다.

## 다음 완료 조건

1. Windows 재부팅 후 영구 등록 장치 유지와 W4b-0 재수신
2. active console 계정으로 새 producer identity manifest를 elevated 설치한 실제 Frame Server handshake·heartbeat 확인
3. CPU latest-frame IPC와 실제 합성 프레임 전달
4. D3D11 공유 텍스처 IPC와 CPU fallback
5. 네이티브 1920×1080 60 FPS 입력 소스로 W1~W3 재검증
6. OBS·SOOP·TikTok LIVE Studio 장치 인식과 실제 영상 수신 W4b
7. 앱·엔진·장치 재시작과 분리·재연결 자동 복구
8. Authenticode signer pin·격리 경계 보강
9. canonical pipe precreation DoS 완화와 RDP·복수 사용자 세션 지원
10. 4시간 1080p60 안정성·드롭·CPU·RAM·VRAM 검증
