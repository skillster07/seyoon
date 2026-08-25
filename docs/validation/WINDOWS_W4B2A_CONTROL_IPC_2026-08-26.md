# Windows W4b-2a control IPC 검증 보고서 — 2026-08-26

## 범위

사용자 세션의 `vividcam_engine`과 Frame Server에서 활성화될 Media Source 사이에
사용할 versioned control channel 기반을 구현했습니다. 이번 단계는 Hello 협상,
cross-process heartbeat, stale·재연결, 보안 경계와 취소 가능한 종료까지입니다.
영상 frame payload와 CPU/D3D11 transport는 포함하지 않습니다.

## 구현 계약

- wire: `VCIP` 1.0, 64-byte little-endian header, 최대 payload 64 KiB
- control: payload 없는 `SourceHello`, `ProducerHello`, `Heartbeat`, `HeartbeatAck`
- endpoint: activation symbolic link의 UTF-16LE SHA-256 digest
- server: 로그인 사용자 세션의 `vividcam_engine`
- client: `IMFMediaSource::Start`가 성공한 뒤 시작되는 source worker
- 보안: protected DACL의 logon SID·LocalService·SYSTEM, remote client 거부,
  첫 Hello 이후 client impersonation·SID 확인·`RevertToSelf`
- source peer gate: server PID, 정확한 `vividcam_engine.exe` basename, 일반 사용자
  token과 pipe session의 자기 일관성을 Hello 전 확인
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

- route 원문 비노출과 SHA-256 golden 값
- current-logon loopback Hello와 peer PID 확인
- 잘못된 magic client 거부 뒤 정상 client 복구
- heartbeat/ack 2회 이상
- client 선기동 뒤 server 연결
- server 종료 뒤 client 재시도
- 같은 endpoint의 server 재시작 뒤 두 번째 handshake
- 동시 start/stop 반복 중 worker·pipe handle 수명 안전성
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

## Portable 검증

WSL Ubuntu GCC 13.3에서 protocol/state tests, portable transport compile과 기존 bounded
engine 실행을 `-Wall -Wextra -Wpedantic -Werror`로 검증했습니다. 모두 종료 코드 0으로
통과했으며 portable engine의 기존 `frame_transport=unavailable` 계약도 유지됐습니다.

## 아직 필요한 로컬 gate

이번 자동 loopback은 current-user peer 경로를 검증합니다. 다음 항목은 machine-wide DLL
재설치가 필요한 실제 Frame Server LocalService 환경에서 확인해야 합니다.

1. 최신 source DLL all-users 재설치
2. 일반 사용자 권한으로 engine 실행
3. 등록 카메라를 2초 이상 열어 handshake와 heartbeat ack 확인
4. engine 종료·재시작 중에도 테스트 패턴이 중단되지 않는지 확인
5. camera Stop·Shutdown 뒤 worker와 pipe handle이 남지 않는지 확인

이 gate가 통과하기 전에는 W4b-2a의 LocalService 실기 검증을 완료로 표시하지 않습니다.
다음 구현 단계는 producer 신원 binding gate를 먼저 추가한 뒤 W4b-2b CPU
latest-frame/backpressure bridge를 연결하는 것입니다.
