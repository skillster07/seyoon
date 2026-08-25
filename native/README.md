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
- RGBA 이미지·텍스트 스타일 리소스 저장소와 GPU 결과 비교용 색상/이미지 CPU 참조 합성기
- 플랫폼 진단 CLI
- 플랫폼 독립 코어 단위 테스트

현재 Windows 구현은 **1080p60 포맷 협상, 비동기 프레임 수신, D3D11·DXGI surface 전달, 단일 카메라 합성 및 W4a 가상 카메라 등록 단계**입니다. 2026-08-26 로컬에서 W1 최선 60 FPS 입력, W2 GPU surface, W3 1080p60 오프스크린 합성·NV12 변환과 W4a COM activation·등록 수명주기가 통과했습니다. 실제 방송 앱이 활성화한 Frame Server 소스로 프레임을 전달하는 W4b producer bridge는 아직 구현 전입니다.

## Linux/macOS 공통 코어 검증

```bash
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/vividcam_diagnostics
```

## Windows 빌드

Visual Studio 2022의 x64 Native Tools Command Prompt에서 실행합니다. 전체 W1/W2 검증은 `native/scripts/validate-windows.ps1`로 한 번에 실행할 수 있습니다.

```powershell
.\native\scripts\validate-windows.ps1

# 또는 단계별 실행
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64
cmake --build native/build --config Release
ctest --test-dir native/build -C Release --output-on-failure
.\native\build\Release\vividcam_diagnostics.exe
.\native\build\Release\vividcam_diagnostics.exe --capture-test
.\native\build\Release\vividcam_diagnostics.exe --render-test
```

검증 스크립트는 `vividcam_virtual_camera_source.dll`의 all-users 설치 상태와 해시를
확인하고 실제 CLSID activation 및 W4a 등록 수명주기까지 검사합니다. Frame Server가
읽을 수 있도록 DLL은 `C:\Program Files\VIVIDCAM\VirtualCamera`에 설치합니다. 수동
설치와 제거는 관리자 PowerShell에서 다음 명령으로 수행할 수 있습니다.

```powershell
.\native\scripts\install-virtual-camera.ps1 -BuildDirectory .\native\build -SkipBuild -AllUsers
.\native\scripts\uninstall-virtual-camera.ps1 -AllUsers
```

Windows 진단 프로그램은 Media Foundation을 초기화하고 연결된 비디오 캡처 장치, 지원 포맷 및 VIVIDCAM이 선택한 우선 포맷을 출력합니다. `--capture-test`는 첫 번째 카메라를 선택 포맷으로 3초간 비동기 캡처하여 수신·소비·덮어쓴 프레임, GPU/CPU 경로와 오류 수를 보고합니다. `--render-test`는 GPU 프레임을 BGRA 렌더 타깃으로 합성하고 p50/p95/max 지연과 16.67ms W3 게이트를 검사합니다.

`--render-test`의 Media Foundation sample 전달은 진단 프로세스 안에서 생성한 source를
대상으로 합니다. OBS·SOOP·TikTok LIVE Studio가 여는 Frame Server source로 실제
프레임을 전달하는지 확인하려면 W4b producer bridge와 별도 호환성 검증이 필요합니다.

## 다음 완료 조건

1. 실제 등록된 가상 카메라 소스의 1920×1080 60 FPS 테스트 패턴 수신
2. 장시간 실행 엔진 호스트와 Frame Server 사이 latest-frame IPC
3. 네이티브 1920×1080 60 FPS 입력 소스로 W1~W3 재검증
4. OBS·SOOP·TikTok LIVE Studio 장치 인식과 실제 영상 수신 W4b
5. 앱·엔진·장치 재시작과 분리·재연결 자동 복구
6. 4시간 1080p60 안정성·드롭·CPU·RAM·VRAM 검증
