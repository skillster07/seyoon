# roughcut: 작업 지침 (Claude Code용)

## 사용자에 대해
- 개발자가 아닙니다. 터미널 명령을 따라 하는 것을 어려워합니다.
- 설명을 나열하지 말고 **직접 실행**하십시오. 파일 받기, 설치, 실행, 오류 수정, 재실행까지 당신이 합니다.
- 사용자에게 요청할 것은 세 가지만입니다: 테스트할 영상 파일 위치, 프리미어에서 열렸는지 확인, 결과물이 쓸 만한지 판단.
- 답변은 한국어, 짧게. 다음에 사용자가 할 일 한 가지만 말합니다.

## 이 프로젝트가 하는 일 (2026-10-11 방향 확정: 팟캐스트)
팟캐스트 원본 녹화 → 트랜스크립트 → **구성안(outline.md)** → 구성안대로 러프컷(`.otio`, FCP 7 XML).
- 구성안은 Claude가 쓰고 사람이 메모장으로 고침. 고친 `outline.md`를 `replan.bat`에 드래그하면 API 호출 없이 타임라인 재생성.
- 모든 플래너는 Outline을 먼저 만들고, 컷은 항상 `outline.outline_to_plan`에서 나옴. 이 구조를 깨지 마십시오.
- 프로파일: `podcast`(기본), `skit`(대본 촬영, 테이크 선별). `profile.txt`로 전환.
- 코드: `roughcut/outline.py`(구성안 모델, md 렌더/파싱), `planner.py`(규칙/Claude, 프롬프트), `cli.py`. README.md에 상세.
- Claude 호출: `client.beta.messages.stream` + `output_format=OutlineModel` + `fallbacks="default"`(beta `server-side-fallback-2026-07-01`), max_tokens 64000, 모델 `claude-opus-5-5`.

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
- 배치 파일에 한글을 쓰지 마십시오. 한국어 Windows cmd는 cp949로 읽어 깨집니다. `.bat`은 ASCII만.
- `.bat`은 CRLF. `.gitattributes`에 설정됨.

## 파일 배치 (2026-10-09 변경)
- 코드 폴더는 어디든 가능. 구글 드라이브 OPENCODE 안도 가능. 한글 경로 때문에 editable install을 쓰지 않고 `PYTHONPATH`로 실행함(`_env.bat`).
- 무거운 것은 `ROUGHCUT_HOME`에: `D:\roughcut`(D 드라이브 있으면) 또는 `%LOCALAPPDATA%\roughcut`. `roughcut_home.txt`로 변경 가능.
  - `venv\` 패키지, `cache\huggingface\` Whisper 모델.
- 결과물은 영상 옆 `영상이름_roughcut\`. 성공 시 `work\*.wav`는 자동 삭제.
- 사용자 C 드라이브 용량 문제의 원인은 코드 폴더 안 `.venv`와 사용자 홈의 Whisper 캐시였음. 새 `setup.bat`이 옛 `.venv`를 지움.

## 사용자 PC에서 실행하는 방법
- 전체 실행: `run.bat`에 영상 드래그. 또는 `_env.bat` 호출 후 `%VENV_PY% -m roughcut run "영상" -o 출력폴더 --language ko --rules rules.podcast.example.md --preview`
- 플래너만 재실행: `replan.bat`에 `analysis.json` 드래그. 또는 `python -m roughcut plan analysis.json -o 출력폴더_v2 --rules ...`
- 테스트: `%VENV_PY% -m pytest -q` (코드 폴더에서, PYTHONPATH 설정 상태)

## 팀 사용 계획
- 코드 폴더를 구글 드라이브 OPENCODE에 두면 팀원 PC에 자동 동기화됨. 각 팀원은 `setup.bat` 한 번(각자 로컬 디스크에 venv), 이후 `run.bat` 드래그.
- `rules.md`는 드라이브로 공유됨. `anthropic_key.txt`도 드라이브에 두면 팀 전체가 키를 공유하는 것이므로 팀 공용 키를 쓰거나 각자 로컬에만 둘 것. 둘 다 gitignore.
- 2단계(감시 폴더, 처리 PC 1대)는 CLI 품질 확인 뒤.

## 다음 할 일 (우선순위)
1. 실제 팟캐스트 녹화 1편으로 첫 실행. 처음엔 10~20분 구간만. 긴 녹화는 CPU Whisper가 오래 걸리므로 subtitle-core SRT를 영상 옆에 두는 것을 권장.
2. `outline.md` 품질 확인: 녹음 전후 잡담 제거, "빼주세요" 제거, 섹션 나눔, 질문-답 분리 여부. 사용자가 손으로 고친 부분을 `rules.md`에 규칙으로 옮김.
3. 3~5편 반복 후 셀렉츠 결과와 비교하여 계속 만들지 결정.
4. 그 다음에만: GPU 설정, 멀티캠, 팀 배포 자동화, UXP 패널.

## 하지 말 것
- UI를 먼저 만들지 마십시오. CLI 품질이 확인된 뒤에만.
- `.prproj`를 직접 생성하거나 파싱하지 마십시오. 비공개 포맷. OTIO/FCP XML만.
- API 키를 코드나 커밋에 넣지 마십시오. `anthropic_key.txt`는 gitignore됨.
