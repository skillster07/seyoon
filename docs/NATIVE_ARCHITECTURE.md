# VIVIDCAM 네이티브 영상 아키텍처

## 목적

VIVIDCAM 네이티브 계층은 카메라 프레임을 장치의 최선 60p 포맷으로 안정적으로 수신하고, 1920×1080 또는 1080×1920 60p 출력 프로필로 정규화한 뒤 GPU에서 뷰티·배경·레이어를 합성하여 Windows 가상 카메라에 전달합니다. React 프로토타입은 사용자 경험 검증용이며 네이티브 코어의 프레임 처리 경로에 포함하지 않습니다.

## 목표 파이프라인

```text
Media Foundation Capture
  → NV12 GPU Surface
  → Face/Body Tracking
  → Beauty + Segmentation
  → Direct3D 11 Layer Compositor
  → Preview Swap Chain
  → Virtual Camera / NDI / Encoder
```

## 모듈 경계

| 모듈 | 책임 | 현재 상태 |
| --- | --- | --- |
| `vividcam_core` | 출력 프로필, 프레임 타이밍, 성능 통계 | 기반 구현 |
| `vividcam_platform` | Windows 장치·세션·GPU 추상화 | 카메라 포맷, 비동기 캡처, D3D11·DXGI Manager 구현 |
| Capture | Media Foundation 소스 리더와 포맷 협상 | GPU DXGI surface 우선·CPU 폴백 비동기 수신 구현 |
| Scene Graph | 장면 템플릿, 레이어 순서·잠금·표시·변환과 렌더 플랜 | 플랫폼 공통 모델·검증·SOOP/TikTok 템플릿 구현 |
| Layer Resources | 이미지 RGBA·텍스트 스타일 리소스와 참조 합성 | 저장소·검증·색상/이미지 CPU 기준 렌더러 구현 |
| Compositor | D3D11 텍스처 및 레이어 합성 | 장면 배경색·카메라 위치/크기/투명도·회전·BGRA 출력 구현, Windows W3 오프스크린 통과, 이미지/텍스트 그리기 예정 |
| Output Hub | 프리뷰·가상 카메라·인코더 소비자 fan-out | 소비자별 latest-frame·덮어쓰기 계측 구현 |
| Effects | 얼굴 추적·뷰티·세그멘테이션 | 예정 |
| Virtual Camera | Windows 카메라 출력 | IMFMediaSource/Stream과 MFCreateVirtualCamera 등록·시작·정지·제거 구현, 로컬 W4a 통과, Frame Server producer bridge 예정 |
| MF Adapter | 타입·샘플·이벤트·descriptor | IMFMediaType, GPU IMFSample, event queue, stream/presentation descriptor와 기본 NV12 선택 구현 |
| Pixel Conversion | 합성 BGRA를 소비자 포맷으로 변환 | CPU 기준 변환과 D3D11 Video Processor NV12 zero-copy·출력 풀 구현, Windows GPU 변환 통과 |
| Engine Host | 사용자 세션 장기 실행·상태 보고 | 별도 `vividcam_engine`, 생명주기·heartbeat·정상 종료와 bounded smoke 구현 |
| Bridge | 데스크톱 UI와 네이티브 명령·상태 연결 | 예정 |

## 프로세스 경계

Windows 방송 앱은 등록된 가상 카메라 COM 서버를 Frame Server의 Local Service
프로세스에서 활성화합니다. 반면 현재 `--render-test`는 진단 프로세스 안에서 별도
Media Source를 만들고 같은 프로세스에서 샘플을 직접 전달합니다. 따라서 로컬에서
확인한 `source_samples`와 `delivered` 수치는 Media Source 계약과 파이프라인의
프로세스 내부 검증이며, 방송 앱의 실제 영상 수신을 뜻하지 않습니다.

W4b에서는 사용자 세션의 엔진과 Frame Server source 사이에 versioned IPC,
latest-frame/backpressure, heartbeat·재연결, producer 부재 시 테스트 패턴을
구현해야 합니다.

W4b-1의 `schema=1` 출력은 엔진 자체의 운영 텔레메트리 형식이며 프로세스 간 wire
protocol은 아닙니다. 다음 IPC 단계에서는 사용자 세션의 엔진을 비동기 named-pipe
server, `IMFMediaSource::Start` 이후의 Frame Server source worker를 client로 둡니다.
`RequestSample`, `ActivateObject`, `DllMain`은 IPC를 기다리지 않으며, 연결 부재·stale
상태에서는 현재 테스트 패턴을 계속 반환합니다. control 메시지는 C++ 메모리 구조를
그대로 전송하지 않고 버전이 명시된 little-endian codec으로 직렬화하며, 원격 연결을
거부하고 로그인 세션과 LocalService만 허용하는 명시적 ACL을 사용합니다.

## 60p 타이밍 원칙

- 기준 주기는 약 16.67ms입니다.
- 캡처, 효과, 합성, 출력 각 구간의 시간을 별도로 측정합니다.
- 완료 시각이 다음 프레임 경계를 넘으면 누락 프레임으로 계측합니다.
- 실시간 방송에서는 오래된 프레임을 누적 처리하지 않고 최신 프레임으로 따라잡습니다.
- SOOP 기본 프로필은 1920×1080 60p, TikTok 기본 프로필은 1080×1920 60p입니다.
- W4b-0 테스트 패턴은 sample timestamp·duration의 논리적 60p 계약을 검증합니다.
- W4b-1 엔진 heartbeat는 steady clock 기반 deadline과 누락 interval을 검증합니다.
  실제 합성 프레임의 wall-clock 60p pacing은 producer IPC를 연결한 뒤 별도로 검증합니다.

## 스레드 모델 초안

1. **Capture thread**: Media Foundation 콜백에서 프레임을 짧게 받아 GPU 큐에 전달합니다.
2. **Render thread**: 최신 프레임에 효과와 레이어를 적용하고 60p deadline에 맞춰 합성합니다.
3. **Output thread**: 가상 카메라·NDI·인코더 소비자에게 완성 프레임을 전달합니다.
4. **Control thread**: UI 명령, 장치 변경, 프로필 변경과 상태 보고를 처리합니다.

## 성능 예산

| 단계 | 1080p60 목표 |
| --- | ---: |
| 카메라 수신·색변환 | 2.0ms |
| 얼굴 추적·뷰티 | 6.0ms |
| 배경 분리 | 4.0ms |
| 레이어 합성 | 2.0ms |
| 출력 전달 | 1.0ms |
| 여유 | 1.67ms |

## 다음 구현 순서

1. Media Foundation 장치 포맷 열거와 1080p60 지원 확인 — 완료
2. 선택한 포맷을 적용하는 Source Reader 비동기 캡처 — 구현, Windows W1 최선 60 FPS 입력 통과, 네이티브 1080p60 입력 예정
3. NV12 → D3D11 텍스처 zero-copy 경로 — 구현, Windows Gate W2 통과
4. GPU 오프스크린 프리뷰 합성과 가로·세로 회전 — 구현, Windows W3 오프스크린 통과, 데스크톱 프리뷰 예정
5. 렌더 p50/p95/max·프레임 누락 텔레메트리 — 구현·로컬 계측 통과, GPU 사용량 추가 예정
6. 플랫폼 공통 장면·레이어 그래프와 렌더 플랜 — 구현
7. 렌더 플랜의 배경색·카메라 변환·투명도를 D3D11 합성기에 연결 — 구현, Windows W3 오프스크린 통과
8. 프리뷰·가상 카메라·인코더용 bounded output fan-out — 구현
9. 이미지·텍스트 리소스 계약과 색상/이미지 CPU 참조 합성기 — 구현
10. D3D11 이미지·텍스트 레이어 렌더러와 데스크톱 프리뷰 연결
11. 가상 카메라 60p 샘플 타임라인·latest-frame·반복 프레임·백프레셔 코어 — 구현
12. Media Source bounded RequestSample 큐·start/stop/shutdown·flush·discontinuity 코어 — 구현
13. SOOP/TikTok 프로필의 NV12/BGRA stride·sample size·60p 미디어 타입 협상 — 구현
14. BT.709 limited-range BGRA→NV12 기준 변환과 2×2 chroma subsampling — 구현
15. D3D11 Video Processor BGRA→NV12 GPU 변환·텍스처 풀·p95 계측 — 구현, Windows W3 통과
16. 협상 타입→IMFMediaType 및 NV12 GPU 텍스처→IMFDXGIBuffer/IMFSample 포장 — 구현, 프로세스 내부 Windows 검증 통과
17. IMFMediaEventQueue 시작·샘플·정지·오류 queue/take/shutdown 브리지 — 구현, 프로세스 내부 검증 통과
18. 복수 IMFMediaType→IMFStreamDescriptor·기본 NV12 선택·IMFPresentationDescriptor 활성화 — 구현, activation 계약 검증 통과
19. Windows IMFMediaSource/IMFMediaStream COM 객체, Start/Stop/Shutdown, bounded RequestSample, GPU event 전달 — 구현, 프로세스 내부 검증 통과
20. MFCreateVirtualCamera 세션/시스템·사용자 접근 등록, Start/Stop/Remove 수명주기 — 구현, Windows W4a 통과
21. IMFActivate COM class factory DLL, all-users 설치·제거·activation probe — 구현, Windows W4a 통과
22. W4b-0 System+CurrentUser 영구 등록, NV12/BGRA 이동 컬러바, symbolic link 기반 실제 Media Foundation consumer smoke — 구현, 로컬 1920×1080 NV12 60p 수신 통과, 재부팅 지속성 대기
23. 장시간 `vividcam_engine` 호스트와 생명주기·heartbeat·텔레메트리 — 구현, Windows 일반 사용자 bounded·Ctrl+C 종료 통과
24. 엔진 사용자 세션 ↔ Frame Server Local Service 사이 versioned control IPC와 CPU latest-frame 브리지
25. D3D11 공유 텍스처 IPC와 CPU fallback, device-lost·재연결 복구
26. OBS → SOOP → TikTok LIVE Studio 장치 열거·1080p60 수신 W4b
