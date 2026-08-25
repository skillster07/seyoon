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

- 2026-08-26 로컬 완료: W1 최선 60 FPS 캡처, W2 GPU surface, W3 1080p60 오프스크린 합성·NV12 변환, W4a COM activation·등록 수명주기, W4b-0 등록 소스 1080p60 테스트 패턴 수신
- 검증 근거: `docs/validation/WINDOWS_W1_W4A_2026-08-26.md`, `docs/validation/WINDOWS_W4B0_2026-08-26.md`
- 입력 한계: 현재 캡처보드 입력은 720×480 60 FPS이며 네이티브 1080p60 입력은 별도 검증 필요
- 현재 핵심 공백: 진단 프로세스의 producer와 Frame Server가 활성화한 Media Source 사이 프레임 브리지가 없음
- 로컬 후속: Windows 재부팅 뒤 W4b-0 영구 등록·재수신 확인
- 클라우드 다음 범위: 장시간 엔진 호스트 → CPU latest-frame IPC → D3D11 공유 텍스처 IPC
- 로컬 다음 상태: OBS에서 실제 등록 장치의 테스트 패턴 수신 후 SOOP·TikTok LIVE Studio까지 1080p60 W4b 확장
- 병행 범위: D3D11 이미지·텍스트 렌더러, 데스크톱 UI bridge, 실제 1080p60 입력 및 장치 매트릭스
