# Windows W4b-2b CPU latest-frame transport core 검증 보고서 — 2026-08-26

## 결과 요약

W4b-2b의 첫 번째 구현 조각으로 control channel 밖에서 1080p60 CPU 프레임을 전달할
수 있는 데이터 평면 코어를 구현했습니다. VCIP 1.0에는 큰 영상 payload를 넣지 않고
`OpenStream`, `TransportOffer`, `TransportAccepted`, `StreamReady`의 작은 고정 크기
little-endian negotiation codec만 추가했습니다. 실제 frame bytes는 source가 생성하고
engine이 여는 별도 두 슬롯 shared-memory latest-frame mailbox에 둡니다.

현재 고정 계약은 **1920×1080, NV12, 60/1p, Y/UV stride 1920, frame payload
3,110,400 bytes**입니다. Windows Release 전체 CTest 10/10, mailbox executable 5회 반복,
WSL GCC의 CPU transport·protocol `-Werror` 빌드와 실행이 통과했습니다.

| 항목 | 상태 |
| --- | --- |
| VCIP compact negotiation codec·strict validation | 통과 |
| Windows Release 전체 빌드·CTest | 통과, 10/10 |
| CPU mailbox Windows 반복 실행 | 통과, 5/5 |
| WSL GCC CPU transport portable stub `-Werror` | 통과 |
| WSL GCC producer IPC protocol `-Werror` | 통과 |
| Windows cross-process latest-frame loopback | 통과 |
| production security의 `Local\` CI seam positive·negative test | 통과 |
| control negotiation·mailbox lifecycle 연결 | 미구현 |
| engine render/readback publisher | 미구현 |
| Media Foundation `RequestSample` consumer·fallback 연결 | 미구현 |
| 설치된 실제 Frame Server·방송 앱 영상 수신 | 미검증 |

따라서 이 보고서는 **transport core 자동 gate의 통과 기록**이며 Gate W4b-2b 전체 완료
판정이 아닙니다. 설치된 카메라의 현재 영상은 계속 W4b-0 컬러바 fallback이고, 실제 엔진
합성 프레임을 전달한다는 뜻이 아닙니다.

## control과 frame data의 분리

VCIP header와 protocol version은 기존 1.0을 유지합니다. 한 장의 packed 1080p NV12
frame은 3,110,400 bytes이므로 64 KiB payload 상한의 heartbeat/control pipe로 보내지
않습니다. control pipe에는 다음 고정 크기 payload만 직렬화합니다.

| payload | 크기 | 역할 |
| --- | ---: | --- |
| `OpenStreamPayload` | 48 bytes | stream ID, 1920×1080 NV12 60/1p, stride·frame bytes 요청 |
| `TransportOfferPayload` | 40 bytes | CPU shared-memory layout와 mapping capacity 제안 |
| `TransportDescriptorPayload` | 40 bytes | `TransportAccepted`·`StreamReady`가 같은 계약을 echo |

codec은 native C++ object representation을 복사하지 않고 명시적 little-endian offset으로
encode/decode합니다. payload size·schema·enum·dimension·frame rate·stride·frame bytes·
slot count·capacity·flag·reserved 값을 strict하게 검사하고, open stream과 offer, offer와
accepted/ready descriptor 사이 계약 불일치도 거부합니다.

이 codec은 다음 control-state 구현에 사용할 기반일 뿐입니다. 현재 engine과 source의
runtime control worker가 위 네 메시지를 교환하거나 mailbox lifetime을 연결하지는 않습니다.

## mailbox ABI와 latest-frame 동작

source/Frame Server 쪽이 mapping을 만들고 engine producer가 이미 초기화된 mapping을 엽니다.
테스트 route는 `Local\`, 설치 경로는 서로 다른 Windows session을 건너기 위해
`Global\` namespace를 사용하도록 이름 계약을 분리했습니다.

```text
Local\VIVIDCAM.Frame.v1.<route-digest>.<connection-id>
Global\VIVIDCAM.Frame.v1.<route-digest>.<connection-id>
```

route 원문은 노출하지 않고 64-hex digest를 사용하며, reconnect는 새 connection ID와 새
mapping을 협상합니다. ABI 크기는 다음과 같습니다.

| 구간 | 크기 |
| --- | ---: |
| mapping header | 4,096 bytes |
| slot metadata | 64 bytes |
| slot NV12 payload | 3,110,400 bytes |
| page-aligned slot span | 3,112,960 bytes |
| slot 수 | 2 |
| 전체 mapping | 6,230,016 bytes |

producer는 inactive slot에 프레임을 복사한 뒤 generation을 atomic publish합니다. shared
header의 CAS claim으로 mapping lifetime당 writer를 하나만 허용하고 두 번째 writer를
거부합니다. consumer는 한 번의 bounded snapshot만 시도하며 overwrite 중인 torn slot을
기다리거나 spin하지 않고 이번 프레임을 건너뜁니다.

프레임마다 ACK를 요구하거나 consumer backpressure로 producer를 대기시키지 않습니다.
느린 consumer가 있으면 중간 frame을 queue하지 않고 최신 frame이 이전 frame을 덮어쓰며,
published·consumed·overwritten·torn·invalid counter로 결과를 계측합니다.

## production mapping 보안 계약

production mapping의 DACL은 상속을 차단하고 정확한 세 개의 direct allow ACE만 허용합니다.

- SYSTEM: `GENERIC_ALL`
- `NT SERVICE\FrameServer`: `GENERIC_ALL`
- 보호된 producer identity manifest의 `EngineUserSid`: `GENERIC_READ | GENERIC_WRITE`

그 밖의 principal, 추가·중복·inherited·deny ACE와 과도하거나 부족한 mask는 거부합니다.
mandatory integrity label은 정확한 **Medium / no-write-up** 한 개만 허용합니다. source 생성과
producer open 양쪽이 DACL과 label을 다시 검사한 뒤에만 mapping을 사용합니다.

일반 Windows CI에서도 이 정책을 직접 검사할 수 있도록 production과 동일한 descriptor를
`Local\` namespace에 적용하는 전용 seam을 두었습니다. positive open/publish/consume뿐
아니라 Everyone 추가 ACE와 Low integrity label을 각각 주입해 producer open이 fail-closed하는
negative test를 실행했습니다. 이 seam의 통과는 설치된 Frame Server가 실제
`Global\` mapping을 생성하고 일반 사용자 engine이 여는 통합 검증을 대신하지 않습니다.

## cross-process 검증

Windows test parent가 source mailbox를 만들고 별도 child process가 producer로 엽니다.
먼저 16개의 서로 다른 packed 1080p NV12 frame을 동기화해 순서와 payload를 확인합니다.
그다음 child는 consumer의 per-frame 대기 없이 140개 frame을 연속 publish하고, parent는
burst가 모두 게시될 때까지 의도적으로 consume하지 않습니다.

검증 결과는 다음 조건을 모두 만족했습니다.

- 전체 156개 frame publish
- synchronized 16개와 burst 최종 1개, 총 17개 consume
- burst 최종 sequence와 정확한 최종 payload 확인
- `overwritten_frames > 0`
- `torn_reads = 0`, `invalid_frames = 0`
- 140-frame producer burst가 5초 bounded budget 안에 완료
- 같은 mapping의 두 번째 writer CAS claim 거부
- 정상 producer close 뒤 writer claim 재획득

별도 counter test는 잘못된 payload 거부, 덮어쓰기 계측, 강제로 만든 torn slot의 bounded
skip·복구, 잘못된 metadata의 invalid 계측·복구와 close 이후 동작도 확인했습니다.

## 자동 검증 결과

현재 branch의 Windows Release CTest는 다음 10개 target을 모두 통과했습니다.

1. core
2. engine
3. control state
4. producer IPC protocol
5. producer identity
6. CPU frame transport
7. control transport
8. registry helper
9. camera update helper
10. engine smoke

CPU frame transport executable은 Windows에서 추가로 5회 연속 통과했습니다. WSL의 GCC
13.3에서는 C++20과 `-Wall -Wextra -Wpedantic -Werror -UNDEBUG` 조건으로 CPU transport
core·portable stub·test와 producer IPC protocol·test를 각각 빌드하고 실행했으며 둘 다
통과했습니다.

## Gate W4b-2b를 닫기 위해 남은 작업

다음 구현과 로컬 통합 검증이 모두 끝날 때까지 Gate W4b-2b는 진행 중입니다.

1. 인증된 VCIP control state에 stream negotiation과 per-connection mailbox 생성·폐기 연결
2. engine의 합성 결과 GPU readback/CPU NV12 publisher 연결과 60p pacing 계측
3. Media Foundation source의 `RequestSample`이 mailbox 최신 frame을 읽고, 부재·torn·stale
   상태에서는 기존 컬러바 또는 마지막 frame을 비차단 반환하도록 연결
4. producer/source 시작 순서, crash, reconnect, connection ID 교체와 stale mapping 회수 검증
5. 설치된 실제 Frame Server에서 `Global\` mapping 보안·cross-session open·프레임 수신 검증
6. OBS, SOOP, TikTok LIVE Studio에서 1920×1080 NV12 60p 실제 합성 영상과 재연결 검증
