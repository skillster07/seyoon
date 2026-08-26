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
| Virtual Camera | Windows 카메라 출력 | IMFMediaSource/Stream과 MFCreateVirtualCamera 등록·시작·정지·제거, 테스트 패턴 fallback, 비차단 control client와 설치 producer identity verifier 구현; CPU mailbox consumer 연결 예정 |
| MF Adapter | 타입·샘플·이벤트·descriptor | IMFMediaType, GPU IMFSample, event queue, stream/presentation descriptor와 기본 NV12 선택 구현 |
| Pixel Conversion | 합성 BGRA를 소비자 포맷으로 변환 | CPU 기준 변환과 D3D11 Video Processor NV12 zero-copy·출력 풀 구현, Windows GPU 변환 통과 |
| Engine Host | 사용자 세션 장기 실행·상태 보고 | `Program Files`에 설치되는 별도 `vividcam_engine`, 생명주기·heartbeat·정상 종료, 비차단 frame worker와 publisher/mailbox 텔레메트리 구현 |
| Control IPC | Engine ↔ Frame Server 제어·상태 | VCIP 1.0 codec, FrameServer service SID 전용 pipe, 설치 사용자 SID·active console·token·경로·SHA-256 identity binding과 heartbeat 재검증, compact frame-transport negotiation·mailbox 수명주기 구현 |
| Frame IPC | Engine → Frame Server 영상 데이터 | 1920×1080 NV12 60/1p 두 슬롯 CPU latest-frame shared-memory, connection별 lifecycle, generation-bound engine publisher 구현; MF consumer 연결 예정 |
| Bridge | 데스크톱 UI와 네이티브 명령·상태 연결 | 예정 |

## 프로세스 경계

Windows 방송 앱은 등록된 가상 카메라 COM 서버를 Frame Server의 Local Service
프로세스에서 활성화합니다. 반면 현재 `--render-test`는 진단 프로세스 안에서 별도
Media Source를 만들고 같은 프로세스에서 샘플을 직접 전달합니다. 따라서 로컬에서
확인한 `source_samples`와 `delivered` 수치는 Media Source 계약과 파이프라인의
프로세스 내부 검증이며, 방송 앱의 실제 영상 수신을 뜻하지 않습니다.

W4b-2a에서는 사용자 세션의 엔진과 Frame Server source 사이 versioned control IPC,
heartbeat·재연결, producer 부재 시 테스트 패턴 fallback과 producer identity binding을
구현했습니다. identity 변경분은 Windows Release 빌드, CTest 9/9, control transport 5회
반복과 Web 검증을 통과했습니다. elevated `validate-windows.ps1`가 generation 1 package를
설치·재검증한 뒤 등록 source의 1920x1080 NV12 60p 샘플 12개를 수신했고, 설치된 일반 사용자
engine과 실제 Frame Server가 handshake 1회와 heartbeat ACK 147/147을 protocol error와
rejected peer 없이 통과했습니다.

W4b-2b의 첫 slice에서는 같은 경계를 위한 compact negotiation codec과 별도 CPU
latest-frame mailbox core를 구현했습니다. Windows Release CTest 10/10, mailbox 5회 반복,
WSL GCC `-Werror` CPU/protocol과 cross-process loopback을 통과했습니다. 다음 slice에서는
bounded control payload I/O, stream negotiation과 connection별 mailbox 수명주기를 engine과
source control worker에 연결했습니다. Windows CTest 10/10, control transport 5/5, mailbox
3/3, 결정적 3,110,400-byte NV12 roundtrip과 wrong-order 거부가 통과했습니다.

W4b-2c에서는 engine renderer publisher를 연결했습니다. 별도 worker가 물리 카메라의
NV12·YUY2·BGRA GPU surface를 선택해 1920×1080 장면 합성, NV12 변환과 CPU readback을
60p로 수행합니다. main engine loop는 mailbox generation별 rational 60p ticket을 만들고
readback 결과를 connection-bound publish API로 전달합니다. worker와 main 사이 payload는
reusable vector swap으로 이동하며 3.1MB frame 복사는 없습니다. 카메라/GPU pipeline은
degraded 시 5초 backoff로 worker thread에서만 재시작하므로 main heartbeat를 막지 않습니다.

Media Foundation `RequestSample` consumer는 아직 연결하지 않아 등록 카메라는 계속 W4b-0
컬러바 fallback을 반환합니다. 설치된 실제 Frame Server의 `Global\` cross-session publisher도
아직 로컬 검증하지 않았습니다.

W4b-1의 `schema=1` 출력은 엔진 자체의 운영 텔레메트리 형식이며 프로세스 간 wire
protocol은 아닙니다. W4b-2a는 사용자 세션의 엔진을 비동기 named-pipe server,
`IMFMediaSource::Start` 이후의 Frame Server source worker를 client로 둡니다.
`RequestSample`, `ActivateObject`, `DllMain`은 IPC를 기다리지 않으며, 연결 부재·stale
상태에서는 현재 테스트 패턴을 계속 반환합니다. control 메시지는 C++ 메모리 구조를
그대로 전송하지 않고 VCIP 1.0 little-endian codec으로 직렬화합니다. producer identity
추가로 wire version·header·payload·message ID를 바꾸거나 HMAC secret을 도입하지
않았습니다.

W4b-2a의 단일 등록 카메라는 source CLSID에 묶인 canonical stable control route를
engine과 source가 공유합니다. activation symbolic link는 Frame Server 경로에서 형태나
전달 여부가 달라질 수 있으므로 IPC 주소로 사용하지 않습니다. route 원문은 pipe 이름에
넣지 않고 UTF-16LE SHA-256 digest만 사용합니다. canonical production pipe의 보호 DACL은
SYSTEM과 정확한 `NT SERVICE\FrameServer` service SID에만 접근을 허용합니다. 서버는 첫
`SourceHello`를 읽은 뒤 client를 impersonate하고, token user가 LocalService인지와 enabled
group에 FrameServer service SID가 있는지를 확인한 뒤 즉시 `RevertToSelf`합니다. 이어
`GetNamedPipeClientProcessId` 결과가 SCM `QueryServiceStatusEx`의 실행 중 FrameServer
PID와 같은지 검사합니다. 일반 사용자, broad LocalService만 가진 process와 다른 service
principal은 production route에 들어올 수 없습니다. 무작위 비canonical route만 loopback
CTest를 위해 기존 SYSTEM·LocalService·현재 logon SID 정책을 유지합니다. 따라서 일반
사용자 `--control-client-test`는 handshake를 시도하지 않고 access denied를 기대하는
negative test입니다. 엔진 pipe가 있을 때 `[control-client-denial] win32=5 [valid]`를
출력하고 종료 코드 0이면 정상입니다.

all-users 설치기는 source DLL·진단 도구와 `vividcam_engine.exe`를
`C:\Program Files\VIVIDCAM\VirtualCamera`에 함께 복사하고 해시를 확인합니다. 설치된
엔진의 identity manifest는
`HKLM\Software\VIVIDCAM\VirtualCamera\ProducerIdentity`에 다음 형식으로 마지막에
기록합니다.

| 값 | Registry 형식 | 계약 |
| --- | --- | --- |
| `SchemaVersion` | `REG_DWORD` | `1` |
| `Generation` | `REG_QWORD` | 유효한 설치마다 증가하는 1 이상의 generation |
| `EnginePath` | `REG_SZ` | 설치된 sibling `vividcam_engine.exe`의 canonical 절대 경로 |
| `EngineUserSid` | `REG_SZ` | all-users 설치를 실행한 elevated 계정의 canonical user SID |
| `EngineSha256` | `REG_BINARY` | 설치된 엔진 파일의 정확한 32-byte SHA-256 |

manifest key는 상속을 차단하고 SYSTEM·Administrators에 `KEY_ALL_ACCESS`, 정확한
FrameServer service SID에 `KEY_QUERY_VALUE | READ_CONTROL`만 주는 세 개의 allow ACE를
가집니다. 그 밖의 deny·inherited·flagged ACE는 허용하지 않으며 owner는 SYSTEM 또는
Administrators여야 합니다. installer와 runtime loader가 값 형식·길이와 이 보안 descriptor를
각각 fail-closed로 검증합니다.

설치기는 교체할 세 파일을 FrameServer 중지 전에 고유한 staging 경로로 복사하고 해시를
확인합니다. 서비스를 멈춘 뒤 기존 target을 backup으로 이동하고 staged 파일을 target으로
교체해 다시 해시를 검증합니다. manifest는 기존 값·보안 descriptor의 정확한 snapshot을
보관한 상태에서 먼저 `Generation=0`을 기록해 in-progress를 표시하고, schema·path·hash·
user SID를 쓴 뒤 최종 generation을 commit marker로 마지막에 기록합니다. 어떤 단계든
실패하면 파일을 역순으로 복원하고 이전 manifest 값·형식·보안 descriptor를 되돌린 뒤
복원 해시까지 검증합니다. 설치된 엔진이 실행 중인데 교체가 필요하면 설치를 거부합니다.
all-users 제거는 영구 카메라를 먼저 제거하고, FrameServer를 멈춘 상태를 유지한 채
manifest·설치 파일·COM 등록을 삭제한 다음 기존 서비스 상태를 복구합니다.

source client는 Hello 전에 pipe server PID, 일반 사용자 token과 pipe session 일치를
확인합니다. canonical route에서는 다음 조건을 모두 요구합니다.

1. engine token user SID가 manifest의 `EngineUserSid`와 같습니다.
2. token session이 pipe session과 같고 `WTSGetActiveConsoleSessionId`가 반환한 현재 물리적
   활성 콘솔 session이며 session 0이 아닙니다.
3. token이 fully elevated가 아니고 integrity level이 medium 이하입니다.
4. 관측 process image 경로가 manifest의 `EnginePath`와 현재 source DLL 옆
   `vividcam_engine.exe` 경로에 모두 일치합니다.
5. 세 경로를 각각 handle로 열었을 때 regular disk file이고 directory·reparse point가
   아니며 `GetFinalPathNameByHandle`로 확인한 최종 경로도 같습니다.
6. 실제 파일 SHA-256이 `EngineSha256`과 상수 시간 비교에서 일치합니다.

manifest 값·DACL, SID, session, elevation·integrity, 경로 또는 해시가 다르면 handshake 전에
실패합니다. 연결 뒤에도 source는 producer heartbeat를 받을 때마다 동일한 token
identity·session·path·hash 검사를 다시 실행하고, 실패하면 ACK를 보내지 않고 연결을
끊어 재연결 상태로 전환합니다. engine은 이 검사에 필요한 범위만 열기 위해 기존
kernel-object DACL을 보존하면서 정확한 FrameServer service SID에 current process의
`PROCESS_QUERY_LIMITED_INFORMATION`과 primary token의 `TOKEN_QUERY`만 direct ACE로
허용합니다. 새 ACE에 다른 process/token 권한이 있거나 설정·재검증이 실패하면 control
server 시작을 거부하며 `TokenDefaultDacl`은 읽거나 변경하지 않습니다. 모든
connect/read/write는 stop event와 함께 overlapped로 기다리고 `CancelIoEx` completion을
회수한 뒤 worker를 종료합니다.

현재 개발 바이너리는 서명되지 않았으므로 이 경로·SHA-256 pin은 설치 시점 code identity를
고정하는 중간 gate입니다. 신뢰 경계에는 Program Files와 HKLM manifest를 바꿀 수 있는
관리자가 포함됩니다. 사용자 쓰기 가능 위치의 동명 복사본과 디스크 파일 교체는 거부하지만,
승인된 같은 사용자 process에 대한 runtime injection·process hollowing을 방지하는 sandbox는
아닙니다. stable canonical pipe 이름을 공격자가 먼저 만드는 availability DoS도 현재 남아
있지만 source는 위장 server의 identity를 거부하므로 권한 획득으로 이어지지는 않습니다.

현 정책은 elevated 설치 계정 SID와 현재 물리적 active console session 한 개를 결합합니다.
RDP-only session, fast user switching과 복수 동시 로그인은 지원하지 않으며 후속 session
broker·registration 설계로 미룹니다. 배포 서명이 준비되면 Authenticode `WinVerifyTrust`와
signer SPKI pin을 추가하고, 위험 모델이 요구하면 restricted broker/package 또는 별도 제한
SID 경계로 producer를 격리합니다. 복수 카메라 또는 다중 session을 지원할 때는 설치 시
생성해 ACL로 보호하는 registration ID를 route와 identity 정책에 포함해야 합니다.

### W4b-2b CPU latest-frame transport와 control lifecycle

packed 1920×1080 NV12 한 장은 3,110,400 bytes이므로 64 KiB payload 상한의 VCIP control
pipe로 보내지 않습니다. VCIP 1.0에는 `OpenStream` 48-byte,
`TransportOffer` 40-byte와 `TransportAccepted`·`StreamReady`가 공유하는 40-byte descriptor
codec만 추가했습니다. encode/decode는 명시적 little-endian offset을 사용하고 size·schema·
format·dimension·rate·stride·capacity·flag·reserved와 메시지 사이 계약 불일치를 거부합니다.
첫 transport stream은 1920×1080 NV12 60/1p, Y/UV stride 1920으로 고정합니다.

실제 frame bytes는 source가 생성하고 engine producer가 여는 두 슬롯 shared-memory
mailbox에 둡니다. 비production route는 `Local\VIVIDCAM.Frame.v1...`, 설치 runtime은
Frame Server session 0과 active user session 사이를 건너는
`Global\VIVIDCAM.Frame.v1...` 이름을 사용합니다. route digest와 per-connection ID를
이름에 포함하며 reconnect는 새 ID와 새 mapping을 사용합니다.

```text
mapping header             4,096 bytes
slot 0 span            3,112,960 bytes
slot 1 span            3,112,960 bytes
total mapping           6,230,016 bytes
```

각 slot은 64-byte metadata와 3,110,400-byte NV12 payload를 담고 4,096-byte 경계로
padding합니다. producer는 inactive slot을 채운 뒤 generation을 atomic publish합니다.
shared CAS claim은 mapping lifetime당 writer 하나만 허용합니다. consumer는 한 번의 bounded
snapshot만 시도하므로 overwrite 중인 torn slot을 기다리거나 spin하지 않습니다. 프레임마다
ACK·queue·backpressure를 두지 않고 최신 frame이 이전 frame을 덮어쓰며 published·consumed·
overwritten·torn·invalid counter를 남깁니다.

production mapping DACL은 상속을 차단하고 정확한 SYSTEM·FrameServer·producer SID 세 direct
allow ACE만 허용합니다. SYSTEM과 `NT SERVICE\FrameServer`는 `GENERIC_ALL`, 보호된 identity
manifest의 producer SID는 `GENERIC_READ | GENERIC_WRITE`만 가집니다. mandatory label은
정확한 Medium/no-write-up이어야 하며 source와 producer가 사용 전에 전체 계약을 재검사합니다.
일반 CI는 production descriptor를 `Local\` namespace에 적용하는 전용 seam으로 정상 경로,
추가 Everyone ACE와 잘못된 Low label의 fail-closed 거부를 검증합니다. 이 seam은 실제
Frame Server가 `Global\` mapping을 생성하는 로컬 통합 gate를 대신하지 않습니다.

cross-process test는 16개 synchronized 1080p frame을 확인한 뒤 consumer의 per-frame 대기
없이 140개를 burst publish합니다. consumer가 burst 동안 읽지 않아도 producer는 5초
bounded budget 안에 완료하고, 마지막 sequence와 payload가 정확하며 overwrite가 발생하고
torn·invalid는 0이어야 합니다.

control worker의 연결 협상은 다음 순서를 사용합니다. 모든 payload는 먼저 64 KiB 상한과
header 계약을 검증한 뒤 고정 크기 codec으로 decode합니다.

```text
SourceHello
  → ProducerHello
  → OpenStream(1920×1080 NV12 60/1p)
  → TransportOffer(CPU shared memory, two slots)
  → TransportAccepted
  → StreamReady
  → Heartbeat / HeartbeatAck
```

source는 offer를 검증한 뒤 route digest와 무작위 connection ID로 mapping을 생성하고
`TransportAccepted`를 보냅니다. source는 이미 검증한 manifest SID로 DACL을 만듭니다.
engine은 exact descriptor를 검증하고 자기 token SID로 그
DACL이 정확히 일치하는지 확인한 뒤 mapping을 writer로 열고 `StreamReady`를 반환합니다. 양쪽
`successful_handshakes`는 `StreamReady`와 mailbox publication이 끝난 뒤 증가하므로 full-ready
session 수를 뜻합니다. wrong-order·sequence·correlation·payload 계약 위반은 mailbox를
게시하지 않고 protocol error로 종료합니다.

현재 mailbox는 control session이 소유합니다. raw mailbox pointer는 외부에 노출하지 않고
generation-bound publish와 gated take·snapshot API만 제공합니다. publish는 caller가 관측한
mailbox object name을 같은 control mutex 아래 다시 비교한 뒤에만 write하므로 disconnect와
새 mapping publication 사이 경쟁에서도 stale frame이 새 generation에 들어가지 않습니다.
disconnect·reconnect·stop·예외 시 close하며,
reconnect는 새 connection ID와 새 object name을 사용합니다. source가 heartbeat stale 상태에
들어가면 take 경로를 suspend하고, producer identity를 다시 검증한 heartbeat를 받은 뒤 같은
session mailbox를 resume합니다. 자동 loopback은 이
lifecycle과 한 장의 정확한 3,110,400-byte NV12 publish/consume을 검증했습니다.

engine GPU readback·CPU NV12 publisher와 60p pacing은 W4b-2c에서 구현했습니다. 다음 구현은
MF `RequestSample` latest-frame consumer와 부재·torn·stale 컬러바/마지막-frame fallback입니다.
설치된 실제 Frame Server가 `Global\` mapping을 만들고 active user engine publisher가 쓰는
로컬 gate는 별도로 통과해야 합니다.

## 60p 타이밍 원칙

- 기준 주기는 약 16.67ms입니다.
- 캡처, 효과, 합성, 출력 각 구간의 시간을 별도로 측정합니다.
- 완료 시각이 다음 프레임 경계를 넘으면 누락 프레임으로 계측합니다.
- 실시간 방송에서는 오래된 프레임을 누적 처리하지 않고 최신 프레임으로 따라잡습니다.
- SOOP 기본 프로필은 1920×1080 60p, TikTok 기본 프로필은 1080×1920 60p입니다.
- W4b-0 테스트 패턴은 sample timestamp·duration의 논리적 60p 계약을 검증합니다.
- W4b-1 엔진 heartbeat는 steady clock 기반 deadline과 누락 interval을 검증합니다.
- W4b-2a cross-process heartbeat는 500ms마다 전송하고 1500ms stale, 3000ms reconnect와
  100→200→400→800→1600→2000ms 제한 backoff를 적용합니다.
- W4b-2b mailbox는 중간 frame을 쌓지 않고 최신 frame으로 따라잡습니다.
- W4b-2c worker와 publisher는 각각 rational 60p deadline을 사용하고, 늦은 작업은 catch-up
  burst 대신 drop으로 기록합니다. 실제 하드웨어 wall-clock 성능은 로컬 gate에서 검증합니다.

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
24. 엔진 사용자 세션 ↔ Frame Server Local Service 사이 versioned control IPC — 구현, Windows loopback·재연결·bounded shutdown과 설치 DLL LocalService handshake·heartbeat 통과
25. installer user SID·active console·non-elevated token, regular non-reparse sibling 경로·SHA-256 manifest와 FrameServer service identity를 묶고 heartbeat마다 재검증하는 producer identity gate — 구현, Windows 자동 검증과 elevated `validate-windows.ps1` generation 1 설치·실제 FrameServer handshake 1회·heartbeat ACK 147/147 통과
26. CPU latest-frame 브리지 — compact negotiation codec·두 슬롯 mailbox core, bounded
    payload I/O와 connection별 create/open·stale·재연결·종료 lifecycle 구현; Windows CTest
    10/10, control 5/5, mailbox 3/3 통과
27. 비압축 카메라 GPU frame의 compositor·NV12 readback, 비차단 60p worker와
    generation-bound CPU mailbox publisher — 구현, Windows CTest 13/13과 핵심 5회 반복 통과;
    설치 FrameServer `Global\` publisher gate 대기
28. MF RequestSample mailbox consumer와 부재·torn·stale fallback
29. D3D11 공유 텍스처 IPC와 CPU fallback, device-lost·재연결 복구
30. OBS → SOOP → TikTok LIVE Studio 장치 열거·1080p60 수신 W4b
