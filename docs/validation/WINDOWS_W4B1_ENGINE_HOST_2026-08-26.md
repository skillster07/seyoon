# Windows W4b-1 엔진 호스트 로컬 검증 보고서 — 2026-08-26

## 범위

일반 사용자 세션에서 별도 `vividcam_engine`이 관리자 권한, 카메라 등록, Media
Foundation 또는 D3D 초기화 없이 장시간 실행할 기반을 검증했습니다. 이번 단계는
생명주기, heartbeat 텔레메트리, 제한 시간 종료와 console signal 정상 종료까지이며,
Frame Server control·프레임 IPC는 포함하지 않습니다.

## 환경

- OS: Windows x64
- 계정: 일반 사용자 계정(로컬 식별자 비공개)
- 실행 권한: 일반 사용자(`IsAdministrator=False`)
- 빌드: Visual Studio 2022, Release x64
- 브랜치: `codex/vividcam-w4b-engine-host`

## 자동 검증

```powershell
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64
cmake --build native/build --config Release --parallel
ctest --test-dir native/build -C Release --output-on-failure
```

결과:

```text
vividcam_core_tests ................. Passed
vividcam_engine_tests ............... Passed
vividcam_engine_bounded_smoke ....... Passed
100% tests passed, 0 tests failed out of 3
```

단위 테스트는 가짜 steady-clock 시각으로 정상·비정상 상태 전환, heartbeat deadline,
여러 interval 지연 시 한 번만 보고하는 backpressure, 누락 interval, clock regression,
첫 종료 사유 유지, terminal uptime 고정과 CLI 오류를 실제 sleep 없이 검사합니다.

## 제한 시간 실행

```powershell
.\native\build\Release\vividcam_engine.exe `
  --run-for-ms 140 --heartbeat-ms 40 --instance-id manual-smoke
```

결과:

```text
[engine] schema=1 event=lifecycle instance=manual-smoke state=starting ...
[engine] schema=1 event=lifecycle instance=manual-smoke state=running ...
[engine] schema=1 event=heartbeat instance=manual-smoke state=running heartbeat_seq=1 uptime_ms=41 ...
[engine] schema=1 event=heartbeat instance=manual-smoke state=running heartbeat_seq=2 uptime_ms=81 ...
[engine] schema=1 event=heartbeat instance=manual-smoke state=running heartbeat_seq=3 uptime_ms=120 ...
[engine] schema=1 event=lifecycle instance=manual-smoke state=stopping ... uptime_ms=140 ... stop_reason=run-for
[engine] schema=1 event=lifecycle instance=manual-smoke state=stopped ... uptime_ms=140 ... stop_reason=run-for
```

종료 코드는 0입니다. `frame_transport=unavailable`은 아직 IPC를 구현하지 않았음을
의도적으로 나타냅니다.

## Windows Ctrl+C 실행

인자 없이 장시간 실행하는 경로와 동일한 경로에서 heartbeat를 확인한 뒤 Ctrl+C를
전달했습니다.

```text
[engine] ... event=heartbeat state=running heartbeat_seq=128 ...
[engine] ... event=lifecycle state=stopping ... stop_reason=signal
[engine] ... event=lifecycle state=stopped ... stop_reason=signal
```

신호 처리기는 원자적 종료 플래그만 기록했고, 25ms polling engine loop가 상태 전환과
출력 flush를 수행한 뒤 종료 코드 0으로 끝났습니다.

## Portable 추가 검증

WSL Ubuntu의 GCC 13.3에서 새 엔진 소스를 `-Wall -Wextra -Wpedantic -Werror`로
직접 컴파일했습니다. engine host 단위 테스트와 100ms bounded 실행이 통과했고,
SIGTERM 전달 시에도 다음 최종 상태와 종료 코드 0을 확인했습니다.

```text
[engine] ... event=lifecycle state=stopping ... stop_reason=signal
[engine] ... event=lifecycle state=stopped ... stop_reason=signal
```

## 판정과 남은 범위

- W4b-1 엔진 호스트 기반: 통과
- W4b-0 실제 1920×1080 NV12 60p 샘플 수신 항목: 통과
- W4b-0 Windows 재부팅 후 영구 등록·재수신: 대기
- 엔진 ↔ Frame Server versioned control·cross-process heartbeat: 다음 단계
- CPU latest-frame IPC와 실제 합성 프레임 전달: 다음 단계
- D3D11 공유 텍스처 IPC, OBS·SOOP·TikTok LIVE Studio: 후속 단계
