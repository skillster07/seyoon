# roughcut: 작업 지침 (Claude Code용)

## 사용자에 대해
- 개발자가 아닙니다. 터미널 명령을 따라 하는 것을 어려워합니다.
- 설명을 나열하지 말고 **직접 실행**하십시오. 파일 받기, 설치, 실행, 오류 수정, 재실행까지 당신이 합니다.
- 사용자에게 요청할 것은 세 가지만입니다: 테스트할 영상 파일 위치, 프리미어에서 열렸는지 확인, 결과물이 쓸 만한지 판단.
- 답변은 한국어, 짧게. 다음에 사용자가 할 일 한 가지만 말합니다.

## 이 프로젝트가 하는 일
원본 영상을 넣으면 프리미어 프로용 러프컷 타임라인(`.otio`, FCP 7 XML)을 만듭니다.
분석(ffprobe, 트랜스크립션, 샷 감지) → 플래너(규칙 또는 Claude) → `.otio` 출력. README.md에 상세.

## 현재 상태 (2026-10-09)
- 사용자 PC: Windows, Python 3.11.9, ffmpeg 8.0.1, NVIDIA GPU 있음(CUDA DLL 미설치). 프리미어 프로 설치됨.
- 코드 위치: `C:\Users\User\seyoon` (ZIP으로 받은 사본, git 저장소 아님). 원격은 GitHub `skillster07/seyoon` 브랜치 `claude/beautiful-cori-6bbj15`.
- `setup.bat` 실행 성공. `.venv` 생성됨.
- 첫 실행 성공: 1분 15초 콩트 촬영본, 규칙 플래너, `small` 모델. 프리미어에서 `.otio` 열림 확인.
- `anthropic_key.txt` 생성됨 → Claude 플래너 활성 상태.
- 마지막 실행: `medium` 모델 + Claude 플래너. 결과 미확인.
- 로컬 사본은 최신 코드가 아닐 수 있음. 작업 시작 시 먼저 GitHub 브랜치와 동기화하십시오 (git이 없으면 ZIP 재다운로드 후 `anthropic_key.txt`, `.venv`는 보존).

## 알려진 문제와 결정
- faster-whisper는 CPU 기본. GPU는 `ROUGHCUT_WHISPER_DEVICE=cuda` + cuBLAS/cuDNN DLL 필요. 아직 설정하지 않음.
- `small` 모델 한국어 오인식 있음("엉기베", "쩔"). `medium`으로 올림. 그래도 고유명사는 틀릴 수 있음.
- 사용자 팀에 자체 자막 도구 **subtitle-core**가 있음(구글 드라이브 OPENCODE 폴더, WhisperX+CUDA). 그 결과 SRT를 영상 옆에 같은 이름으로 두면 `run.bat`이 자동 사용. 장기적으로 트랜스크립션은 subtitle-core가 담당.
- 배치 파일 안 `if` 블록의 `echo`에 괄호를 쓰지 마십시오. 블록이 깨져 창이 즉시 닫힙니다.
- `.bat`은 CRLF. `.gitattributes`에 설정됨.

## 사용자 PC에서 실행하는 방법
- 전체 실행: `run.bat`에 영상 드래그. 또는 `.venv\Scripts\python.exe -m roughcut run "영상" -o 출력폴더 --language ko --rules rules.example.md --preview`
- 플래너만 재실행: `replan.bat`에 `analysis.json` 드래그. 또는 `python -m roughcut plan analysis.json -o 출력폴더_v2 --rules ...`
- 테스트: `.venv\Scripts\python.exe -m pytest -q`

## 다음 할 일 (우선순위)
1. 마지막 `medium` + Claude 플래너 실행 결과(`*_roughcut\report.md`) 확인. 반복 테이크 중 하나만 골랐는지, 섹션 제목이 말이 되는지.
2. 틀린 결정을 `rules.md`에 규칙으로 적고 `replan.bat`으로 재실행. 이 반복이 핵심 루프.
3. 영상 3~5개로 반복한 뒤 셀렉츠(상용 도구) 결과와 비교하여 계속 만들지 결정.
4. 그 다음에만: GPU 설정, 감시 폴더 방식(팀 배포), UXP 패널.

## 하지 말 것
- UI를 먼저 만들지 마십시오. CLI 품질이 확인된 뒤에만.
- `.prproj`를 직접 생성하거나 파싱하지 마십시오. 비공개 포맷. OTIO/FCP XML만.
- API 키를 코드나 커밋에 넣지 마십시오. `anthropic_key.txt`는 gitignore됨.
