# Windows W4b-2b control·mailbox lifecycle 검증 보고서 — 2026-08-26

## 결과 요약

VCIP control session에 bounded payload I/O, 고정 1920×1080 NV12 60/1p stream 협상과
connection별 CPU latest-frame mailbox 수명주기를 연결했습니다. source가 mapping을 만들고
engine이 single writer로 연 뒤에만 session을 full ready로 공개합니다.

자동 검증 결과는 다음과 같습니다.

| 항목 | 결과 |
| --- | --- |
| Windows Release CTest | 통과, 10/10 |
| control transport 반복 | 통과, 5/5 |
| CPU mailbox 반복 | 통과, 3/3 |
| control session의 3,110,400-byte NV12 roundtrip | 통과, exact payload |
| wrong-order negotiation | 통과, fail-closed 거부 |
| stale suspend·verified heartbeat resume | 구현·state-machine 검증, mailbox 통합 gate 대기 |
| disconnect·reconnect·stop close와 새 reconnect 이름 | 통과 |
| 설치된 실제 Frame Server의 `Global\` cross-session mapping | 미검증 |
| engine renderer publisher | 미구현 |
| MF `RequestSample` mailbox consumer | 미구현 |

따라서 이 변경은 **control negotiation과 mailbox lifecycle 자동 gate 통과**입니다. 실제
합성 영상 전달이나 Gate W4b-2b 전체 완료 판정은 아닙니다.

첫 transport core의 ABI·보안·burst 검증 기록은
`WINDOWS_W4B2B_CPU_FRAME_TRANSPORT_2026-08-26.md`에 그대로 유지합니다.

## Control 협상

VCIP 1.0의 64-byte header와 64 KiB 최대 payload 계약은 유지합니다. frame bytes는 control
pipe로 보내지 않고, bounded reader가 header 검증 뒤 선언된 작은 payload만 정확히 읽습니다.
협상 순서는 다음과 같습니다.

```text
Frame Server source                         User-session engine
       |                                            |
       | SourceHello(connection ID)                 |
       |------------------------------------------->|
       |                              ProducerHello |
       |<-------------------------------------------|
       | OpenStream(1920x1080 NV12 60/1p)           |
       |------------------------------------------->|
       |                              TransportOffer|
       |<-------------------------------------------|
       | create per-connection mailbox              |
       | TransportAccepted                          |
       |------------------------------------------->|
       |                    open mailbox as producer|
       |                                 StreamReady|
       |<-------------------------------------------|
       | Heartbeat / HeartbeatAck                   |
```

`OpenStream`은 48 bytes, `TransportOffer`은 40 bytes,
`TransportAccepted`·`StreamReady` descriptor는 각각 40 bytes입니다. 각 단계에서 message
type, connection ID, 증가하는 sequence, correlation, flags, payload 길이와 stream/transport
exact contract를 검증합니다. 순서가 바뀌거나 descriptor가 offer를 변경하면 mapping을 ready로
게시하지 않고 protocol failure로 연결을 닫습니다.

`successful_handshakes`는 단순 Hello 교환 직후가 아니라 `StreamReady` 검증과 현재 mailbox
publication이 끝난 뒤 증가합니다. 따라서 이 변경 이후 `successful_handshakes >= 1`은 해당
endpoint에서 full-ready negotiation을 한 번 이상 완료했다는 뜻입니다.

## Mailbox 수명주기

source는 canonical route의 SHA-256 digest와 source가 생성한 무작위 connection ID로 object
name을 만들고 mailbox를 생성합니다. engine은 같은 값과 설치 manifest의 producer SID를
가리키는 자기 token SID를 사용해 mapping 보안·ABI·connection ID를 다시 검사하고 writer
claim을 획득합니다. source가 DACL에 넣는 SID는 handshake 전 검증한 manifest에서 옵니다.

- 비production 자동 test: `Local\VIVIDCAM.Frame.v1...`
- 설치 runtime: `Global\VIVIDCAM.Frame.v1...`
- frame contract: packed 1920×1080 NV12, 3,110,400 bytes
- mapping contract: 4,096-byte header + 3,112,960-byte slot 두 개, 총 6,230,016 bytes

control session은 source·producer mailbox의 소유자입니다. raw mailbox pointer는 공개하지
않고 mutex-gated `publish_cpu_frame`·`take_latest_cpu_frame`과 snapshot/name만 제공합니다.
session disconnect, reconnect, stop 또는 worker 예외가 발생하면 underlying mailbox를 닫습니다.
reconnect는 새 connection ID와 새 object name을 사용하므로 이전 writer나 stale mapping을
재사용하지 않습니다.

source가 heartbeat stale 상태에 들어가면 gated take 경로를 suspend합니다. producer
identity·session·path·hash를 다시 검증한 heartbeat가 도착하면 같은 session의 take 경로를
resume합니다. reconnect threshold에 도달하면 해당 mailbox를 닫고 새 session을 시작합니다.

자동 control loopback은 mailbox가 ready인 동안 결정적인 3,110,400-byte NV12 frame 한 장을
publish·consume하고 sequence, timestamp, format과 전체 payload가 정확한지 비교했습니다.
이 검증은 control negotiation과 data-plane object가 실제로 연결됐음을 확인하지만 engine
renderer나 Media Foundation sample 경로를 실행하지는 않습니다.

## Engine 상태와 현재 영상

engine heartbeat는 negotiated mailbox snapshot이 open이면 다음처럼 표시합니다.

```text
[engine] ... frame_transport=ready ...
```

이 값은 mailbox가 협상·open됐다는 의미입니다. engine 합성 결과가 publish됐거나 등록 카메라가
그 frame을 consume했다는 의미가 아닙니다. session이 없거나 stale·reconnect 중이면
`frame_transport=unavailable`로 돌아갑니다.

현재 engine renderer는 producer mailbox에 frame을 쓰지 않고 MF `RequestSample`도 source
mailbox를 읽지 않습니다. 그러므로 등록 카메라가 기존 1920×1080 NV12 60p 이동 컬러바를
반환하는 것이 정상입니다.

## 설치된 `Global\` mapping 로컬 검증

자동 test는 비production `Local\` route와 production security CI seam을 사용합니다. 실제
Frame Server session 0이 `Global\` mapping을 만들고 active console user의 설치 engine이
여는 cross-session gate는 별도 로컬 검증이 필요합니다.

먼저 설치된 engine을 모두 종료합니다. 같은 active console 계정의 관리자 64-bit
PowerShell에서 repository root로 이동한 뒤 전체 검증·재설치를 실행합니다.

```powershell
.\native\scripts\validate-windows.ps1
```

관리자 창의 작업이 끝나면 일반 사용자 PowerShell 창 1에서 설치된 engine을 실행합니다.

```powershell
$engine = Join-Path $env:ProgramFiles "VIVIDCAM\VirtualCamera\vividcam_engine.exe"
& $engine
```

일반 사용자 PowerShell 창 2에서 등록 source를 엽니다.

```powershell
$diag = Join-Path $env:ProgramFiles "VIVIDCAM\VirtualCamera\vividcam_diagnostics.exe"
& $diag --registered-source-test
```

진단이 끝나면 창 1에서 Ctrl+C로 engine을 종료합니다. 통과 기준은 다음과 같습니다.

- diagnostics: `[registered-source] samples=12 ... 1920x1080 NV12 60/1p ... [valid]`
- engine: `successful_handshakes >= 1`
- engine: `heartbeats_sent == heartbeat_acks`
- engine: `protocol_errors=0`, `rejected_peers=0`
- consumer를 engine heartbeat 시점까지 열어 두면 `frame_transport=ready`

12-frame 진단이 첫 500ms control heartbeat 전에 끝나면 heartbeat `0/0`이거나 engine lifecycle
출력에서 순간적인 `ready` 행을 보지 못할 수 있습니다. full-ready 뒤 증가하는
`successful_handshakes >= 1`과 오류 0이 핵심 gate입니다. 출력 영상은 이 단계에서 컬러바가
정상입니다.

이 보고서 작성 시점에는 위 실제 `Global\` 실행 결과를 받지 않았으므로 해당 항목을 통과로
판정하지 않습니다.

## 다음 권장 개발 순서

1. 설치된 실제 Frame Server의 `Global\` create/open과 engine/source 종료·재시작·재연결 확인
2. engine 합성 결과의 GPU readback·CPU NV12 mailbox publisher와 60p pacing·drop 계측 연결
3. MF `RequestSample` latest-frame consumer 연결
4. frame 부재·torn·stale에서 컬러바 또는 마지막 정상 frame을 비차단 반환하는 fallback 연결
5. 설치된 camera에서 실제 합성 frame 확인 후 OBS → SOOP → TikTok LIVE Studio 호환·재연결 검증
6. CPU fallback gate 뒤 D3D11 공유 텍스처 IPC 추가
