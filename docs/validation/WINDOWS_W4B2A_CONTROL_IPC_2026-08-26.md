# Windows W4b-2a control IPC 검증 보고서 — 2026-08-26

## 범위

사용자 세션의 `vividcam_engine`과 Frame Server에서 활성화될 Media Source 사이에
사용할 versioned control channel 기반을 구현했습니다. 이번 단계는 Hello 협상,
cross-process heartbeat, stale·재연결, 보안 경계와 취소 가능한 종료까지입니다.
영상 frame payload와 CPU/D3D11 transport는 포함하지 않습니다.

## 구현 계약

- wire: `VCIP` 1.0, 64-byte little-endian header, 최대 payload 64 KiB
- control: payload 없는 `SourceHello`, `ProducerHello`, `Heartbeat`, `HeartbeatAck`
- endpoint: 단일 VIVIDCAM source CLSID에 묶인 stable route의 UTF-16LE SHA-256 digest
- server: 로그인 사용자 세션의 `vividcam_engine`
- client: `IMFMediaSource::Start`가 성공한 뒤 시작되는 source worker
- 보안: protected DACL의 logon SID·LocalService·SYSTEM, remote client 거부,
  첫 Hello 이후 client impersonation·SID 확인·`RevertToSelf`
- source peer gate: server PID, 정확한 `vividcam_engine.exe` basename, 일반 사용자
  token과 pipe session의 자기 일관성을 Hello 전 확인
- engine peer-inspection grant: 기존 object DACL을 보존하면서 LocalService 직접 ACE에
  process `PROCESS_QUERY_LIMITED_INFORMATION`, primary token `TOKEN_QUERY`만 추가하고 실패 폐쇄
- 종료: stop event, `CancelIoEx`, overlapped completion 회수, worker join
- fallback: control 부재·오류·재연결 중에도 기존 1080p60 테스트 패턴 유지

현재 peer gate는 잘못된 서비스나 우발적인 pipe 선점을 줄이는 방어 계층이며 암호학적
상호 인증은 아닙니다. W4b-2a는 payload와 handle을 전송하지 않으므로 이 범위에서
제어 채널 기반만 병합합니다. W4b-2b에서 frame payload 또는 공유 handle을 허용하기
전에는 패키지 code-signature 확인 또는 ACL로 보호된 카메라별 nonce challenge를
필수 보안 gate로 추가합니다. 잘못된 nonce·서명·실행 경로, service token과 다른
session peer가 거부되는 negative test도 함께 통과해야 합니다.

## 자동 Windows 검증

```powershell
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64
cmake --build native/build --config Release --parallel
ctest --test-dir native/build -C Release --output-on-failure
```

결과:

```text
vividcam_core_tests ........................ Passed
vividcam_engine_tests ...................... Passed
vividcam_control_channel_state_tests ....... Passed
vividcam_producer_ipc_protocol_tests ....... Passed
vividcam_control_channel_transport_tests ... Passed
vividcam_engine_bounded_smoke .............. Passed
100% tests passed, 0 tests failed out of 6
```

transport test는 다음을 실제 Windows named pipe 두 worker로 검사합니다.

- stable single-camera route 원문 비노출과 SHA-256 golden 값
- current-logon loopback Hello와 peer PID 확인
- 잘못된 magic client 거부 뒤 정상 client 복구
- heartbeat/ack 2회 이상
- client 선기동 뒤 server 연결
- server 종료 뒤 client 재시도
- 같은 endpoint의 server 재시작 뒤 두 번째 handshake
- 동시 start/stop 반복 중 worker·pipe handle 수명 안전성
- LocalService process/token query ACE의 정확한 mask, 금지 권한 부재와 반복 start 멱등성
- client/server stop 합계 2초 미만

동일 transport 실행 파일을 5회 연속 실행해 모두 통과했습니다.

## 실제 별도 프로세스 검증

등록된 카메라 route를 사용해 서로 다른 일반 사용자 프로세스의 production peer gate도
검증했습니다.

```powershell
.\native\build\Release\vividcam_engine.exe --run-for-ms 60000 --quiet
.\native\build\Release\vividcam_diagnostics.exe --control-client-test
```

두 번째 명령의 결과:

```text
[control-client] handshakes=1 heartbeat_acks=2 protocol_errors=0 rejected_peers=0 [valid]
```

이 검증은 실제 engine image·token·session 확인을 통과한 것이며 아래의 Frame Server
LocalService source 검증을 대체하지 않습니다.

## 첫 LocalService gate 관찰과 route 보정

최신 설치 DLL과 build DLL의 SHA-256 일치, HKLM COM 경로와 LocalService FrameServer
재기동을 확인한 상태에서 등록 카메라의 컬러바는 정상 수신됐습니다. 그러나 최초 실제
gate의 engine 종료 telemetry는 다음과 같았습니다.

```text
connection_attempts=1 successful_handshakes=0 heartbeats_sent=0
heartbeat_acks=0 protocol_errors=0 rejected_peers=0
```

server의 `connection_attempts=1`은 한 pipe instance가 `ConnectNamedPipe`에서 기다렸다는
뜻입니다. 정확히 같은 endpoint에 `CreateFile`이 성공했다면 client가 Hello 전에 닫혀도
server가 다음 instance를 만들기 때문에 25초 실행에서 값이 2 이상이어야 합니다. 따라서
설치 실패나 컬러바 source 실패가 아니라 activation symbolic link 기반 rendezvous가 실제
Frame Server 경로에서 성립하지 않은 것으로 판단했습니다.

보정 후 engine과 source는 단일 source CLSID 기반 stable route를 직접 공유합니다. 등록
endpoint와 nonempty symbolic link 검증은 engine 시작 조건으로 유지하지만 symbolic link
문자열 자체를 pipe 주소로 사용하지 않습니다. 보정된 별도 일반 사용자 프로세스 검증은
다시 `handshakes=1 heartbeat_acks=2 ... [valid]`로 통과했습니다.

Frame Server의 source peer gate는 LocalService가 engine process와 primary token을
query해야 하므로, 재검증 전에 engine이 두 kernel object의 기존 DACL을 보존하며 각각
`PROCESS_QUERY_LIMITED_INFORMATION`과 `TOKEN_QUERY`만 LocalService에 부여하도록
보강했습니다. 새 direct ACE에는 process 종료·메모리 접근, token duplicate·impersonate
권한을 넣지 않으며 `TokenDefaultDacl`도 변경하지 않습니다. 적용 또는 재검증 실패 시
control server는 시작하지 않습니다.

이 검사는 기본 사용자 token DACL 계약의 LocalService direct ACE를 대상으로 합니다.
그룹·callback/object ACE를 별도로 구성한 hardened/custom DACL 환경은 후속 호환성
매트릭스에서 검증합니다. token DACL 단계가 실패하면 server는 시작되지 않지만 앞서
추가된 process query ACE는 조회 전용 상태로 engine process 종료 때까지 남을 수 있습니다.

또한 canonical route로 실행한 engine과 build의 synthetic `IMFMediaSource`를 별도
프로세스로 연결해 `successful_handshakes=1`, protocol/rejected 오류 0을 확인했습니다.
이 source smoke는 3개 샘플 직후 종료되므로 heartbeat ACK gate는 아래의 2초 이상 실제
Frame Server 실행에서 확인합니다.

## 설치 DLL LocalService gate 재검증 — 통과

최신 DLL을 all-users로 재설치한 뒤 일반 사용자 engine과 실제 등록 카메라를 연결했습니다.
등록 source의 컬러바 수신과 함께 engine 종료 telemetry는 다음과 같았습니다.

```text
[engine-control] schema=1 event=stopped running=false connected=false
connection_attempts=1 successful_handshakes=1 heartbeats_sent=69
heartbeat_acks=69 protocol_errors=0 rejected_peers=0
```

한 번의 연결 시도에서 handshake가 성공했고 69개 heartbeat가 모두 ACK됐습니다.
`connection_attempts=1`은 이 실행에서 한 연결이 유지됐다는 뜻이며 실패가 아닙니다.
`running=false connected=false`도 engine 종료 뒤의 최종 snapshot이므로 정상입니다.
protocol 오류와 peer 거부가 모두 0이므로 stable route, LocalService pipe 접근과
engine process/token peer inspection을 포함한 설치 DLL gate를 통과했습니다.
`frame_transport=unavailable`은 W4b-2b 영상 frame bridge 전의 의도된 상태입니다.

## Portable 검증

WSL Ubuntu GCC 13.3에서 protocol/state tests, portable transport compile과 기존 bounded
engine 실행을 `-Wall -Wextra -Wpedantic -Werror`로 검증했습니다. 모두 종료 코드 0으로
통과했으며 portable engine의 기존 `frame_transport=unavailable` 계약도 유지됐습니다.

## Gate 판정과 다음 범위

W4b-2a의 Windows loopback, 별도 일반 사용자 process와 설치 DLL Frame Server
LocalService 실기 gate가 모두 통과했습니다. 자동 테스트의 engine 선·후기동, server
재시작·재연결과 bounded Stop·Shutdown 계약도 유지됩니다.

다음 구현 단계는 producer 신원 binding gate를 먼저 추가한 뒤 W4b-2b CPU
latest-frame/backpressure bridge를 연결하는 것입니다. 실제 방송 앱의 반복 종료·재시작과
handle 회수는 OBS·SOOP·TikTok LIVE Studio W4b 호환 매트릭스에서도 계속 검증합니다.
