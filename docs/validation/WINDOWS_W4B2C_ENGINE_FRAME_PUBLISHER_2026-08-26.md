# Windows W4b-2c engine frame publisher 검증 보고서 — 2026-08-26

## 결과 요약

사용자 세션 engine에 physical camera GPU capture → 1920×1080 compositor → NV12 converter →
CPU readback → negotiated mailbox publisher 경로를 연결했습니다. Windows 자동 검증은
통과했으며, 새 package와 실제 Frame Server `Global\` mapping을 사용하는 로컬 publisher
gate는 대기 중입니다.

| 항목 | 결과 |
| --- | --- |
| Windows Release build | 통과 |
| Windows Release CTest | 통과, 13/13 |
| publisher·worker·control transport 반복 | 통과, 각 5/5 |
| portable worker GCC strict warning 반복 | 통과, 10/10 |
| worker ASan·UBSan | 통과 |
| engine `--run-for-ms 350` 생명주기 | 통과, heartbeat 3회·종료 코드 0 |
| 실제 설치 Frame Server `Global\` publisher | 미검증 |
| MF mailbox consumer·실제 합성 영상 표시 | 미구현, 컬러바 fallback 유지 |

따라서 이 변경의 판정은 **engine publisher 구현·자동 gate 통과, 로컬 하드웨어 gate 대기**입니다.

## 프레임 경로

```text
physical camera (NV12 / YUY2 / BGRA GPU surface)
  → SOOP 1920×1080 scene compositor
  → D3D11 Video Processor NV12 conversion
  → reusable D3D11 staging readback
  → reusable packed 3,110,400-byte CpuNv12Frame
  → rational 60p publisher ticket
  → generation-bound two-slot shared-memory publish
```

MJPEG·H264-only 장치는 현재 decode 범위가 아니므로 선택하지 않습니다. 첫 카메라가 사용할 수
없어도 다음 physical camera를 순회합니다. GPU-compatible 포맷이 없거나 capture/GPU stage가
지속 실패하면 pipeline은 degraded 원인을 기록하고 worker가 제한된 backoff로 재생성합니다.

D3D11 staging texture와 CPU vector는 재사용합니다. worker latest slot과 main publisher frame은
vector를 swap하므로 3.1MB payload를 추가 복사하지 않습니다. shared-memory writer는 inactive
slot에 한 번 복사하고 최신 generation을 atomic publish합니다.

## 스레드·복구 경계

카메라 열거·Media Foundation open, compositor/converter와 synchronous staging `Map`은 모두
전용 frame worker에서 실행합니다. engine main thread는 control 상태, heartbeat, publisher
deadline과 종료 신호만 처리합니다.

- mailbox 없음: worker pipeline 비차단 disable
- mailbox 연결: worker enable, publisher는 pipeline 준비와 무관하게 60p ticket 시작
- 준비 전/복구 중: `NoInput` 계측, control heartbeat 유지
- 지속 no-frame 또는 stage 실패: degraded 판정 뒤 기본 5초 backoff 재시작
- disconnect: 새 frame 노출 즉시 중단, 실제 camera stop은 worker에서 수행
- engine 종료: publisher disable → control server stop → frame worker join 순서

worker가 예기치 않게 종료되면 engine은 계속 running/NoInput으로 남지 않고 runtime failure로
전환합니다. Media Foundation 비동기 종료는 `OnFlush`, 활성 callback 0, reader의 callback
reference 분리를 순서대로 확인합니다. 드라이버가 이 경계를 2초 안에 완료하지 못하면 늦은
callback의 use-after-free를 피하도록 해당 캡처 수명을 프로세스 종료까지 보존하고, 추가 reader
생성을 차단합니다. 이 비정상 상태의 복구에는 engine 프로세스 재시작이 필요합니다.

## 60p publisher 계약

publisher는 steady clock epoch에서 정확한 rational 60p deadline을 계산합니다. logical sequence
1의 timestamp는 0이며 sequence 601은 정확히 100,000,000(100ns)입니다. 늦은 시작이나 긴
readback/publish 뒤 지난 deadline은 catch-up burst로 만들지 않고 drop으로 기록합니다.

새 camera frame이 없으면 마지막 정상 CPU frame을 재사용하고 pipeline generation과 원 capture
sequence의 쌍으로 repeated를 계측합니다. 따라서 degraded 복구로 capture sequence가 다시
시작돼도 첫 프레임은 반복으로 오판하지 않습니다. reconnect는 repeat 기준만 초기화하며 logical
output sequence·timestamp와 누적 통계는 계속 증가합니다.

## Reconnect 안전성

mailbox object name에는 무작위 connection ID가 들어가므로 connection generation 식별자로
사용합니다. main은 frame 작업 전에 name을 캡처합니다. control server는 같은 mutex 아래에서
현재 name과 예상 name을 비교한 뒤에만 publish합니다.

이 때문에 readback 도중 old session이 닫히고 새 mapping이 게시되어도 old ticket은
`MailboxChanged`로 거부되며 새 mapping에 쓸 수 없습니다. 실제 server stop/start로 새 이름을
만든 테스트가 이전 이름의 publish 거부, 새 source의 무수신, 이후 정상 exact roundtrip을
검증합니다. generation을 확인하지 않는 우회 publish API는 제거했습니다.

## 텔레메트리

`[engine-frame]`은 다음 process 누적·현재 pipeline 수치를 함께 출력합니다.

- publisher due/published/repeated/deadline drop/NoInput/transport/publish failure
- worker pipeline attempts/restarts/produced/consumed/overwritten/no-frame/deadline drop
- current pipeline capture/render/conversion/readback counters와 readback p95
- mailbox overwrite와 publish p95
- 현재 state, mailbox readiness와 마지막 degraded 오류

`[engine] frame_transport=ready`는 mailbox 협상·open만 뜻합니다. 실제 engine write는
`[engine-frame] published>0`으로 판정합니다. MF consumer가 아직 없으므로 등록 카메라 화면은
이 단계에서 컬러바가 정상입니다.

## 로컬 Windows gate

먼저 설치된 engine을 종료하고 관리자 64-bit PowerShell에서 repository root로 이동해 새
Release package를 빌드·설치합니다.

```powershell
.\native\scripts\validate-windows.ps1
```

완료 후 같은 active console 계정의 일반 사용자 PowerShell 창 1에서 설치 engine을 실행합니다.

```powershell
$engine = Join-Path $env:ProgramFiles "VIVIDCAM\VirtualCamera\vividcam_engine.exe"
& $engine
```

일반 사용자 PowerShell 창 2에서 실제 Frame Server session을 약 10초 유지합니다.

```powershell
$diag = Join-Path $env:ProgramFiles "VIVIDCAM\VirtualCamera\vividcam_diagnostics.exe"
& $diag --registered-source-hold-test
```

두 번째 명령은 현재 source 출력인 컬러바 600개의 1080p60 계약을 검증할 뿐 publisher
provenance를 단독으로 증명하지 않습니다. 창 1의 engine telemetry를 함께 확인해야 합니다.

통과 기준:

- diagnostics: `[registered-source-hold] samples=600 ... 1920x1080 NV12 60/1p ... [valid]`
- engine: `successful_handshakes >= 1`
- engine: `[engine-frame] state=ready mailbox=ready`와 `published > 0`
- engine: `heartbeats_sent == heartbeat_acks`, `protocol_errors=0`, `rejected_peers=0`
- Ctrl+C 종료 뒤 physical camera를 다른 앱에서 다시 열 수 있음

이 로컬 결과를 받기 전에는 실제 `Global\` publisher를 통과로 판정하지 않습니다.

## 다음 개발 순서

1. 위 설치 runtime publisher gate와 disconnect/reconnect 반복
2. MF `RequestSample`에서 latest mailbox frame을 NV12 sample로 반환
3. frame 부재·torn·stale에서 마지막 정상 frame 또는 컬러바를 비차단 반환
4. 설치 camera에서 실제 physical/composited 영상 확인
5. OBS → SOOP → TikTok LIVE Studio 수신·재연결
6. CPU fallback 검증 뒤 D3D11 shared-texture IPC
