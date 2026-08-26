# Windows W4b-2a producer identity binding 검증 보고서 — 2026-08-26

## 결과 요약

W4b-2a control channel에 installer-pinned producer identity gate를 구현했습니다. Windows
Release 빌드, CTest 7/7, control transport 5회 반복과 Web production build는
통과했습니다. 이 결과에는 installer user SID가 포함된 manifest 구조, 파일 SHA-256,
regular non-reparse 설치 sibling 경로와 mismatch 거부를 직접 검사하는
`vividcam_producer_identity_tests`가 포함됩니다.

새 identity manifest를 관리자 권한으로 실제 설치한 뒤 설치 엔진과 Windows FrameServer를
연결하는 로컬 통합 항목은 아직 실행하지 않았습니다. 따라서 이 보고서는 구현·자동 검증
통과를 기록하며, 새 설치 환경의 FrameServer handshake·heartbeat를 통과했다고 주장하지
않습니다.

| 항목 | 상태 |
| --- | --- |
| Windows Release 전체 빌드 | 통과 |
| Windows CTest | 통과, 7/7 |
| control transport 반복 | 통과, 5/5 |
| direct manifest/hash/path verifier | 통과 |
| Web production build | 통과 |
| elevated `validate-windows.ps1` package current/repair 재검증 | 구현, 실제 관리자 실행 대기 |
| 새 manifest 설치 후 실제 FrameServer integration | 대기, elevated validation/repair와 사용자 통합 확인 필요 |

## 신원 기준

현재 개발 바이너리는 Authenticode 서명이 없으므로 일반 사용자 엔진이 읽을 수 있는 공유
secret이나 per-camera HMAC을 신원 기준으로 사용하지 않습니다. 같은 사용자 권한의 위장
process도 그런 secret을 읽을 수 있기 때문입니다. 이번 단계는 관리자 설치기가 보호한
경로·파일 SHA-256·설치 계정 SID와 Windows FrameServer service identity를 함께 묶는
중간 gate입니다.

all-users 설치기는 다음 파일을 같은 보호 디렉터리에 배치하고 복사 후 해시를 확인합니다.

```text
C:\Program Files\VIVIDCAM\VirtualCamera\vividcam_virtual_camera_source.dll
C:\Program Files\VIVIDCAM\VirtualCamera\vividcam_diagnostics.exe
C:\Program Files\VIVIDCAM\VirtualCamera\vividcam_engine.exe
```

identity manifest 위치와 값 계약은 다음과 같습니다.

```text
HKLM\Software\VIVIDCAM\VirtualCamera\ProducerIdentity
```

| 값 | 형식 | 조건 |
| --- | --- | --- |
| `SchemaVersion` | `REG_DWORD` | 정확히 `1` |
| `Generation` | `REG_QWORD` | `1` 이상, 유효한 갱신에서 증가 |
| `EnginePath` | `REG_SZ` | 설치된 `vividcam_engine.exe`의 canonical 절대 경로 |
| `EngineUserSid` | `REG_SZ` | all-users 설치를 실행한 elevated 계정의 canonical user SID |
| `EngineSha256` | `REG_BINARY` | 정확히 32-byte SHA-256 |

manifest key의 DACL은 상속을 차단하며 다음 세 개의 allow ACE만 허용합니다.

- SYSTEM: `KEY_ALL_ACCESS`
- BUILTIN\Administrators: `KEY_ALL_ACCESS`
- `NT SERVICE\FrameServer`: `KEY_QUERY_VALUE | READ_CONTROL`

owner는 SYSTEM을 우선하고 Windows privilege 상태 때문에 불가능하면 Administrators를
허용합니다. runtime loader는 보호 DACL, owner, ACE 수·종류·mask·flag와 registry 값의
형식·길이를 다시 검사하고 하나라도 다르면 fail-closed합니다.

설치기는 교체할 source DLL·diagnostics·engine을 FrameServer 중지 전에 고유한 stage
파일로 복사하고 SHA-256을 확인합니다. 그다음 서비스를 멈추고 다음 transaction을
실행합니다.

1. 기존 target이 있으면 고유한 backup 이름으로 이동합니다.
2. 검증된 stage 파일을 target으로 이동하고 세 target의 SHA-256을 다시 확인합니다.
3. manifest의 이전 값·형식·보안 descriptor snapshot을 유지한 상태에서
   `Generation=0`을 먼저 기록해 reader가 in-progress 상태를 거부하게 합니다.
4. `SchemaVersion`, `EnginePath`, `EngineSha256`, `EngineUserSid`를 기록하고 flush합니다.
5. 1 이상의 최종 `Generation`을 마지막 commit marker로 기록하고 전체 계약을 검증합니다.

중간 실패 시 파일을 역순으로 이전 위치에 복원하고 기존 manifest 값·형식·보안
descriptor를 정확히 복원하며, 복원된 각 파일 해시와 manifest snapshot까지 확인합니다.
rollback이 완전하지 않으면 남긴 backup 경로와 실패를 명확히 보고합니다. 교체가 필요한
설치 엔진이 실행 중이면 transaction 전 종료를 요구합니다. all-users 제거는 영구 카메라를
먼저 제거한 뒤 FrameServer를 멈추고 manifest·설치 파일·COM 등록을 모두 삭제할 때까지
서비스를 다시 시작하지 않습니다.

## production control gate

canonical production pipe는 SYSTEM과 정확한 `NT SERVICE\FrameServer` service SID에만
접근을 허용합니다. engine server는 연결한 client를 impersonate해 다음을 모두 확인합니다.

1. token user가 LocalService입니다.
2. enabled group에 FrameServer service SID가 있습니다.
3. `GetNamedPipeClientProcessId` 결과가 SCM `QueryServiceStatusEx`가 보고한 실행 중
   FrameServer PID와 같습니다.
4. impersonation 검사가 끝나면 모든 경로에서 즉시 `RevertToSelf`합니다.

engine은 FrameServer가 producer를 검사하는 데 필요한 권한만 직접 제공합니다.

- current process: `PROCESS_QUERY_LIMITED_INFORMATION`
- primary token: `TOKEN_QUERY`

두 ACE의 principal은 broad LocalService가 아니라 정확한 FrameServer service SID입니다.
기존 kernel-object DACL은 보존하고 다른 권한을 추가하지 않으며 `TokenDefaultDacl`은
변경하지 않습니다.

source client는 handshake 전 engine server에 다음 순서의 검사를 적용합니다.

1. named-pipe server PID와 process image 경로를 읽습니다.
2. server가 LocalService·LocalSystem·NetworkService가 아닌 일반 사용자 principal인지와
   token session이 pipe session과 같은지 확인합니다.
3. 보호된 HKLM manifest를 읽고 DACL·owner·다섯 값 계약을 확인합니다.
4. engine token user SID가 manifest `EngineUserSid`와 같은지 확인합니다.
5. token session이 session 0이 아닌 현재 `WTSGetActiveConsoleSessionId`와 같은지
   확인합니다. 현 단계는 물리적 active console user 하나만 지원합니다.
6. `TokenElevationType`이 fully elevated가 아니며 token integrity RID가 medium 이하인지
   확인합니다.
7. 관측한 process image 경로, manifest `EnginePath`, 현재 source DLL 옆의
   `vividcam_engine.exe` 예상 경로가 문자열 수준에서 모두 같은지 확인합니다.
8. 세 경로를 각각 handle로 열어 regular disk file이고 directory·reparse point가 아닌지
   검사하고, `GetFinalPathNameByHandle`로 해석한 최종 경로도 모두 같은지 확인합니다.
9. 관측한 파일의 SHA-256을 다시 계산하고 manifest의 32-byte digest와 상수 시간으로
   비교합니다.

이 전체 producer 검사는 handshake 전 한 번으로 끝나지 않습니다. source는 producer
heartbeat를 받을 때마다 PID·token user SID·active console session·elevation·integrity·
manifest·resolved path·SHA-256을 다시 확인합니다. 재검증 실패 시 heartbeat ACK를 보내지
않고 현재 연결을 끊어 재연결 상태로 돌아갑니다.

canonical route에는 in-process 또는 일반 사용자 diagnostics 우회 경로가 없습니다.
비canonical 무작위 route만 transport CTest를 위해 기존 SYSTEM·LocalService·현재 logon
SID loopback 정책을 유지합니다.

## protocol 호환성

producer identity binding은 VCIP 1.0의 64-byte little-endian header, message ID, payload,
sequence·correlation 규칙을 변경하지 않습니다. wire에 shared secret, nonce challenge 또는
HMAC을 추가하지 않았습니다. 기존 heartbeat·stale·재연결·bounded shutdown 상태 기계는
그대로 두고 canonical 연결 승인과 각 heartbeat ACK 앞에 OS identity와 installed image
검사를 추가했습니다.

## 자동 검증 결과

Windows Release 전체 CTest 7개가 통과했습니다. 새 direct identity test는 다음 positive와
negative 경로를 실행했습니다.

- 현재 test executable의 SHA-256 계산 성공
- 유효한 schema·generation·path·user SID·hash manifest의 image 검증 성공
- 한 byte가 다른 SHA-256 거부
- 설치 sibling과 다른 expected path 거부
- generation 0인 invalid manifest 거부
- 현재 module 옆 `vividcam_engine.exe` 경로 계산

control transport test는 FrameServer SID 전용 production 정책에서의 일반 사용자
canonical client 거부, process/token의 최소 direct ACE와 idempotence를 포함하며 5회
연속 통과했습니다. 이 테스트는 무작위 route loopback과 실제 Windows service
activation을 구분합니다.

## 전체 Windows 검증의 설치 판정

`native/scripts/validate-windows.ps1`는 이제 보호된 machine producer identity를 직접
확인하고 필요하면 설치를 repair하므로 elevated 64-bit PowerShell에서만 실행됩니다.
validation process의 현재 user SID를 이번 설치가 허용할 `EngineUserSid`로 사용합니다.

스크립트는 다음 조건을 전부 만족할 때만 all-users package를 current로 판정합니다.

1. HKLM CLSID `InprocServer32` 기본값이
   `C:\Program Files\VIVIDCAM\VirtualCamera\vividcam_virtual_camera_source.dll`을 정확히
   가리킵니다.
2. 설치된 source DLL·`vividcam_diagnostics.exe`·`vividcam_engine.exe`가 모두 존재하고,
   세 파일의 SHA-256이 현재 Release build artifact와 각각 같습니다.
3. producer manifest에 `SchemaVersion`, `Generation`, `EnginePath`, `EngineSha256`,
   `EngineUserSid`만 정확히 존재하며 각 registry 형식과 값·길이가 계약에 맞습니다.
4. schema는 1, generation은 1 이상, engine path는 설치된 engine 경로,
   `EngineUserSid`는 현재 elevated validation 계정 SID, 32-byte `EngineSha256`은 설치
   engine의 실제 해시입니다.
5. manifest owner는 SYSTEM 또는 Administrators이고 DACL 상속이 차단되어 있습니다.
   allow ACE는 SYSTEM·Administrators FullControl과 FrameServer
   QueryValues·ReadPermissions의 정확한 세 개뿐이며 inherited·deny·추가 ACE가 없습니다.

하나라도 다르면 스크립트가 `install-virtual-camera.ps1 -SkipBuild -AllUsers`를 호출해
package를 repair/reinstall하고 위 다섯 조건을 다시 전부 검사합니다. 두 번째 검사도
실패하면 나머지 hardware gate를 진행하지 않습니다. repair가 필요할 때 설치 engine이
실행 중이면 안전한 교체가 거부되므로 먼저 engine을 종료해야 합니다.

이 검사는 COM 경로·파일·manifest가 현재 build와 일치함을 보장하지만, 로그인 사용자로
engine을 실행해 실제 FrameServer와 handshake·heartbeat했다는 뜻은 아닙니다. 그 통합
항목은 아래 절차로 별도 확인합니다.

## 남은 로컬 통합 gate

먼저 카메라를 사용하는 앱을 닫습니다. 설치 상태가 current인지 확실하지 않다면 실행 중인
VIVIDCAM engine도 종료합니다. engine을 실행할 현재 active console 사용자와 같은 Windows
계정으로 **64-bit PowerShell을 UAC elevation**한 뒤 전체 검증을 실행합니다. 다른 관리자
자격 증명으로 실행하면 manifest SID가 그 관리자에게 다시 binding되어 로그인 사용자의
engine이 production에서 거부됩니다.

```powershell
Set-Location "C:\Users\User\Documents\Codex\2026-07-30\prior-conversation-with-codex-conversation-role\work\vividcam-local"
.\native\scripts\validate-windows.ps1
```

package만 수동으로 설치해야 할 때는 같은 elevated 64-bit PowerShell에서 다음 명령을 사용할
수 있습니다.

```powershell
.\native\scripts\install-virtual-camera.ps1 -BuildDirectory ".\native\build" -SkipBuild -AllUsers
```

그다음 같은 active console 계정의 일반 사용자 PowerShell에서 build 복사본이 아니라
설치된 engine을 실행합니다. 관리자 PowerShell에서는 실행하지 않습니다.

```powershell
$engine = Join-Path $env:ProgramFiles "VIVIDCAM\VirtualCamera\vividcam_engine.exe"
& $engine
```

engine pipe가 열린 뒤 별도 일반 사용자 PowerShell에서 production DACL의 expected-denial
진단을 실행합니다.

```powershell
.\native\build\Release\vividcam_diagnostics.exe --control-client-test
```

이 명령은 이제 handshake가 아니라 access-denied negative test입니다. pipe가 존재할 때
다음 출력과 종료 코드 0이 통과 기준입니다.

```text
[control-client-denial] win32=5 [valid]
```

`pipe unavailable` 또는 `[invalid]`는 통과가 아닙니다. 그다음 등록 카메라를 진단 consumer나
OBS에서 열어 실제 FrameServer source를 활성화합니다. 통과 기준은 설치된 engine의
`successful_handshakes`와 `heartbeat_acks`가 지속 증가하고 protocol 오류나 rejected
production peer가 없으며, consumer가 계속 샘플을 받는 것입니다. heartbeat마다 전체
identity/path/hash 재검증이 실행되므로 여러 ACK가 연속 증가해야 합니다. 새 설치 통합
gate가 통과한 뒤 이 보고서에 실제 환경과 handshake·heartbeat 수치를 추가합니다.

## 한계와 후속 보강

설치 경로·SHA-256 pin은 사용자 쓰기 가능 위치의 동명 복사본, basename spoof, reparse
redirect와 설치 후 디스크 파일 교체를 막습니다. 남은 경계는 다음과 같습니다.

- `Program Files`와 HKLM manifest를 변경할 수 있는 local administrator는 신뢰 경계 안입니다.
- 승인된 같은 사용자 process에 대한 runtime code injection과 process hollowing은 현재
  범위 밖입니다.
- stable canonical pipe 이름을 먼저 생성해 정상 engine의 pipe 생성을 막는 availability
  DoS가 남아 있습니다. source는 위장 server를 거부하므로 이것이 identity 우회로 이어지지는
  않습니다.
- `WTSGetActiveConsoleSessionId` 기준의 물리적 active console user 한 명만 지원합니다.
  RDP-only, fast user switching과 복수 동시 session은 후속 broker·registration 설계로
  미룹니다.
- 새 elevated install과 실제 FrameServer handshake·heartbeat 성공은 여전히 로컬 검증
  대기이며 이번 자동 결과로 대체되지 않습니다.

배포 서명 이후 Authenticode `WinVerifyTrust`와 허용 signer의 SPKI pin을 추가하고, 위협
모델이 요구하면 restricted broker/package 또는 별도 제한 SID 경계를 도입합니다.

다음 기능 구현 순서는 W4b-2b CPU latest-frame/backpressure bridge와 실제 합성 프레임
전달입니다. 그 뒤 D3D11 공유 텍스처 IPC와 CPU fallback, producer/source 재시작 복구,
OBS·SOOP·TikTok LIVE Studio 호환 gate로 확장합니다.
