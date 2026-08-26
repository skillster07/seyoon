# VIVIDCAM 제품 개발 로드맵

## 제품 목표

TikTok LIVE Studio, OBS, SOOP에서 바로 사용할 수 있으며 YYCam Pro보다 설정이 쉽고, 한국 크리에이터에게 더 자연스러운 실시간 뷰티·배경·인터랙션 경험을 제공하는 상용 Windows 가상 카메라 제품을 출시한다.

## 개발 원칙

1. **방송 안정성 우선**: SOOP을 포함한 주력 방송 환경에서 60 FPS를 기본으로 유지하고 장애 복구를 먼저 검증한다.
2. **로컬 영상 처리 우선**: 얼굴 원본 영상은 기본적으로 PC 밖으로 전송하지 않는다.
3. **플랫폼 비종속**: TikTok을 시작점으로 하되 가상 카메라, NDI, RTMP를 공통 출력 계층으로 둔다.
4. **점진적 네이티브 전환**: 웹 UX 프로토타입으로 흐름을 검증하고 Windows 영상 엔진을 독립 모듈로 구축한다.
5. **측정 가능한 출시 기준**: 각 단계는 성능, 안정성, 전환율 지표를 통과해야 완료된다.

## Phase 1 — UX 및 수요 검증 (네이티브 개발과 병행)

- 장면·소스·레이어 워크플로
- 세로·가로 캔버스
- 뷰티·색보정·배경 설정
- 카메라 미리보기
- 프로젝트 자동 저장
- TikTok/OBS/SOOP 출력 준비 흐름

**완료 기준**

- 핵심 과업 사용성 테스트 성공률 85% 이상
- 신규 사용자가 5분 안에 첫 장면 구성
- 10명 이상의 한국 라이브 크리에이터 인터뷰 및 피드백 반영

## Phase 2 — Windows 네이티브 기반

**상태: 진행 중** — C++20 공통 코어, Media Foundation 60p 캡처, D3D11·DXGI GPU surface, Video Processor 오프스크린 합성, 텍스처 풀과 렌더 지연 계측 기반을 구축했습니다. Windows CI와 로컬 W1/W2/W3 하드웨어 게이트로 검증합니다.

**2026-08-26 엔지니어링 기준선:** W1 최선 60 FPS 캡처, W2 GPU surface,
W3 1080p60 오프스크린 합성·NV12 변환, W4a COM activation·등록 수명주기가
로컬 Windows에서 통과했습니다. W4b-0 영구 등록 소스도 실제 Frame Server
consumer에서 1920×1080 NV12 60p 이동 컬러바 12개와 정확한 logical cadence를
전달했습니다. 일반 사용자 세션의 W4b-1 `vividcam_engine`도 장시간 생명주기,
heartbeat, 제한 시간·Ctrl+C 정상 종료 기반을 갖췄습니다. 재부팅 지속성,
실제 1080p60 입력과 CPU latest-frame publisher·consumer 연결,
OBS·SOOP·TikTok LIVE Studio 수신 W4b는 남아 있습니다.

기본 W4b-2a versioned control은 VCIP 1.0 codec, cross-process heartbeat와 자동
재연결까지 구현했고 Windows loopback gate를 통과했습니다. 기존 설치 Frame Server
LocalService와도 handshake 1회, heartbeat ACK 69회를 오류 없이 통과했습니다.

그 뒤 frame transport 전 producer identity gate를 구현했습니다. all-users 설치기가
`vividcam_engine.exe`를 source DLL과 같은 `Program Files` 디렉터리에 설치하고, 보호된
HKLM manifest에 generation·절대 경로·SHA-256과 elevated installer 계정 SID를 고정합니다.
파일은 FrameServer 중지 전에 stage·hash하고, 중지 뒤 transactionally 교체하며
`Generation=0` in-progress marker와 최종 generation commit을 사용합니다. 실패 시 이전 파일과
manifest를 정확히 rollback합니다. uninstall도 COM 등록을 지울 때까지 FrameServer를
중지합니다.

canonical pipe는 SYSTEM과 FrameServer service SID만 허용하고 LocalService user·enabled
service SID·SCM PID를 함께 검증합니다. source는 engine token user가 installer SID인지,
현재 active console session의 non-elevated·medium 이하 token인지 확인합니다. process image,
설치 sibling과 manifest 경로가 regular non-reparse file로 같은 최종 경로를 가리키는지와
파일 SHA-256을 handshake 전뿐 아니라 매 heartbeat마다 다시 확인합니다. engine의
process/token에는 FrameServer SID의 최소 조회 권한만 추가합니다. 이 변경은 Windows Release
CTest 9/9, control transport 5회 반복과 Web 검증을 통과했습니다. elevated
`validate-windows.ps1`의 generation 1 package 설치·재검증과 등록 source 1920x1080 NV12 60p
수신 뒤, 설치된 일반 사용자 engine과 실제 Frame Server가 handshake 1회와 heartbeat ACK
147/147을 protocol error·rejected peer 없이 통과했습니다. 실제 합성 frame data-plane 연결은
후속 범위입니다.

W4b-2b의 첫 transport core로 VCIP compact negotiation codec과 고정
1920×1080 NV12 60/1p CPU mailbox를 구현했습니다. 큰 frame은 control pipe 밖의 두 슬롯
latest-frame shared memory에 두며, 4,096-byte header와 3,112,960-byte slot 두 개를 합쳐
전체 6,230,016 bytes입니다. shared CAS로 writer 하나만 허용하고 per-frame ACK나
backpressure 없이 최신 frame이 이전 frame을 덮어씁니다. production mapping은 SYSTEM·
FrameServer·producer SID의 exact protected DACL과 Medium/no-write-up label을 요구합니다.
Windows Release CTest 10/10, mailbox 5회 반복, WSL GCC `-Werror` CPU/protocol과 16-frame
synchronized + 140-frame burst cross-process test가 통과했습니다.

다음 slice에서는 bounded payload I/O와
`SourceHello → ProducerHello → OpenStream → TransportOffer → TransportAccepted → StreamReady`
협상을 control worker에 연결했습니다. source가 connection별 mapping을 만들고 engine이
single writer로 연 뒤에만 full-ready handshake를 기록합니다. heartbeat stale 시 source
take 경로를 중단하고 검증된 heartbeat 뒤 복구하며, disconnect·reconnect·stop에서는 양쪽
mailbox를 닫고 reconnect마다 새 connection ID·새 object name을 사용합니다. Windows CTest
10/10, control transport 5/5, mailbox 3/3, 결정적 3,110,400-byte NV12 roundtrip과
wrong-order 거부가 통과했습니다.

W4b-2c에서는 물리 카메라의 비압축 GPU 입력을 SOOP 1920×1080 장면으로 합성하고 NV12로
변환한 뒤 D3D11 staging readback을 통해 negotiated mailbox에 게시하는 engine publisher를
연결했습니다. 카메라·GPU 호출은 별도 60p worker에 격리하여 control heartbeat와 엔진
종료 루프를 막지 않으며, degraded 상태는 5초 backoff로 다시 시작합니다. publisher는
rational 60p timestamp, backlog drop, 반복 입력과 실패를 계측합니다. reconnect마다 mailbox
이름을 generation으로 사용하고 이름 비교와 publish를 같은 잠금 아래 수행하므로 이전
연결용 frame이 새 mapping에 들어가지 않습니다. Windows Release CTest 13/13과 worker·
publisher·control 각 5회 반복이 통과했습니다.

MF `RequestSample` consumer/fallback은 아직 연결하지 않아 등록 카메라는 계속 컬러바를
반환합니다. 설치된 실제 Frame Server의 `Global\` mapping에 실제 engine frame이 게시되는
로컬 gate와 방송 앱 수신도 아직 검증하지 않았으므로 Gate W4b는 진행 중입니다.

기존 VCIP 1.0 header·version과 prior message 계약은 유지하고 frame bytes 대신 compact
negotiation payload만 추가했습니다. unsigned 개발 빌드에는 설치 경로·SHA-256 pin을
중간 신뢰 기준으로 사용하고, 배포 서명이 준비되면 Authenticode signer SPKI pin과 필요 시
restricted broker/package 경계를 추가합니다. 현재 Program Files·HKLM 관리자 경계를
신뢰하며 same-user runtime injection·process hollowing과 canonical pipe precreation DoS는
해결하지 않습니다. active console session 하나만 지원하고 RDP·복수 동시 session은
후속 범위입니다.

- Windows 카메라 캡처(Media Foundation)
- Direct3D 기반 GPU 합성
- Media Foundation 가상 카메라
- 720p/1080p, 기본 60 FPS 출력(저사양 호환 모드만 30 FPS)
- 카메라 점유 충돌 감지 및 복구
- 프로젝트 파일 저장·복원
- 크래시 리포팅과 장치 진단

**완료 기준**

- 1080p60 연속 4시간 방송에서 프레임 드롭 1% 미만
- 지원 GPU 환경에서 CPU 평균 25% 이하
- TikTok LIVE Studio, OBS, SOOP 호환 테스트 통과
- 비정상 종료 후 프로젝트 복구율 99%

## Phase 3 — AI 뷰티 및 배경 엔진

- 얼굴 랜드마크 및 추적
- 피부·얼굴형·눈·턱 보정
- 인물 세그멘테이션과 가상 배경
- 다인 얼굴 추적
- 뷰티 프리셋 저장·공유
- 저조도·빠른 동작·가림 상황 품질 개선

**완료 기준**

- 효과 활성화 상태에서 1080p60 실시간 유지
- 얼굴 추적 지연 50ms 이하
- 테스트 사용자 자연스러움 선호도에서 YYCam Pro 대비 우위
- 원본 얼굴 영상 로컬 처리 및 개인정보 검토 통과

## Phase 4 — 방송 제작 기능

- 화면·게임·창 캡처
- 이미지·영상·텍스트·브라우저 소스
- 장면 전환과 단축키
- 크로마키
- 스마트폰 Wi-Fi/USB 카메라
- NDI 입력·출력
- 오디오 믹서와 효과음 패드

## Phase 5 — 한국형 라이브 인터랙션

- TikTok 댓글·선물·팔로우 이벤트 수신
- 알림·목표 진도바·TTS
- 댓글/선물 투표
- 팀 PK·룰렛·미션
- SOOP 및 YouTube 이벤트 확장
- 금칙어·스팸·악성 채팅 필터

**정책 게이트**

- 공식 API 및 플랫폼 약관 검토
- 선물 기반 게임의 사행성·청소년 보호 법률 검토
- 계정 토큰 암호화와 삭제 정책 검증

## Phase 6 — 상용화

- 무료/Pro/Agency 요금제
- 국내 결제 및 전자영수증
- 리소스·플러그인 마켓
- 팀 프리셋과 원격 지원
- 자동 업데이트와 롤백
- 한국어 고객센터 및 장비 호환 데이터베이스

## 품질 지표

| 영역 | 핵심 지표 |
| --- | --- |
| 성능 | FPS, 렌더 지연, GPU/CPU/메모리 사용량 |
| 안정성 | 크래시 없는 방송 시간, 장치 복구율 |
| 영상 품질 | 얼굴 추적 실패율, 마스크 흔들림, 사용자 선호도 |
| 사용성 | 첫 방송 준비 시간, 과업 성공률, 설정 이탈률 |
| 사업성 | 활성 방송자, 주간 방송 시간, 유료 전환, 해지율 |
| 지원 | 문의 해결 시간, 장치별 실패율, 환불률 |

## 다음 구현 백로그

1. Windows 재부팅 후 W4b-0 영구 등록·재수신 확인
2. active console 계정으로 새 producer identity manifest를 elevated 설치한 실제 FrameServer handshake·heartbeat 확인 — 완료
3. 설치된 Frame Server의 `Global\` CPU mapping, producer/source 재시작·재연결 검증
4. CPU latest-frame IPC — codec·mailbox·authenticated control lifecycle 자동 검증 완료;
   engine render/readback CPU NV12 publisher·60p worker/pacing 자동 검증 완료, 설치 runtime
   publisher 로컬 gate 대기
5. MF `RequestSample` latest-frame consumer와 부재·torn·stale 컬러바/마지막-frame fallback
6. D3D11 공유 텍스처 IPC와 CPU fallback
7. Authenticode signer SPKI pin과 producer 격리 경계 보강
8. canonical pipe precreation DoS 완화와 RDP·복수 사용자 session broker 설계
9. OBS → SOOP → TikTok LIVE Studio W4b 호환·재연결 검증
10. 실제 1080p60 입력과 지원 장치 매트릭스(NVIDIA/AMD/Intel, 캡처 카드, 웹캠)
11. 4시간 기본 파이프라인 안정성 기준선
12. 얼굴 추적·세그멘테이션 SDK 자체 개발/라이선스 비교
13. 웹 프로토타입 사용성 테스트와 이벤트 로깅은 네이티브 트랙과 병행
