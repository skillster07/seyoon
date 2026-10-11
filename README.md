# roughcut

팟캐스트 원본 녹화를 넣으면 **구성안**을 먼저 쓰고, 그 구성안대로 자른 **러프컷 타임라인**을 프리미어 프로용 파일(`.otio`, FCP 7 XML)로 만드는 도구입니다.

편집기를 대체하지 않습니다. 원본을 다 듣고 구성을 짜고 쓸 구간을 고르는 첫 단계를 덜어 주는 도구입니다. 마무리는 프리미어에서 합니다.

```
원본 녹화 ─▶ 트랜스크립트 ─▶ 구성안 (outline.md) ─▶ 러프컷 (roughcut.otio) ─▶ 프리미어
             Whisper 또는        Claude가 작성          구성안대로 자동 생성
             subtitle-core SRT   사람이 메모장으로 수정 ─┘ 수정하면 무료로 다시 생성
```

## 작업 흐름

1. 원본 영상을 `run.bat` 위로 드래그합니다. 끝나면 결과 폴더와 메모장이 열립니다.
2. 메모장에 열린 `outline.md`가 구성안입니다. 섹션 제목, 길이, 넣은 구간, 뺀 구간과 이유, 쇼츠 후보가 들어 있습니다.
3. 구성안이 마음에 들면 `roughcut.otio`를 프리미어에서 File, Import로 엽니다. 섹션마다 마커가 있습니다.
4. 구성안을 고치고 싶으면 메모장에서 고쳐 저장한 뒤, `outline.md`를 `replan.bat` 위로 드래그합니다. API를 다시 부르지 않고 고친 그대로 타임라인을 다시 만듭니다. 비용이 없습니다.
5. 구성을 처음부터 다시 받고 싶으면 `rules.md`를 고친 뒤 `analysis.json`을 `replan.bat` 위로 드래그합니다. 이때는 Claude를 다시 부릅니다.

결과는 원본 폴더를 덮어쓰지 않고 `_v2`, `_v3` 폴더로 새로 생깁니다.

### 구성안 고치는 법

```
## 1. 콜드 오픈                     ← 섹션. 순서를 바꾸면 타임라인 순서가 바뀝니다
> 길이: 0분 29초 / 목적: ... / 이유: ...
- #812-#820 [01:03:12-01:04:40] ...  ← 넣을 구간. 줄을 지우면 빠지고, 다른 섹션으로 옮기면 그리로 갑니다
- 빼기: #815                        ← 구간 안에서 잘라낼 번호

## 제외                              ← 타임라인에 안 들어가는 섹션. 여기 줄을 위로 옮기면 살아납니다
## 쇼츠 후보                         ← 참고용
```

번호와 대사는 같은 폴더의 `transcript.txt`에서 찾습니다. 필러만 있는 발화("음", "어")는 자동으로 빠집니다.

## Windows 설치 (터미널 없이)

요구 사항: Python 3.10 이상, ffmpeg. 확인은 PowerShell에서 `python --version`, `ffmpeg -version`.

1. 이 저장소를 받습니다. 브랜치는 `claude/beautiful-cori-6bbj15`. 코드 폴더는 구글 드라이브 안에 두어도 됩니다.
2. `setup.bat`을 더블클릭합니다. 패키지와 Whisper 모델은 코드 폴더가 아니라 로컬 디스크 `D:\roughcut`(D 드라이브가 없으면 `%LOCALAPPDATA%\roughcut`)에 설치됩니다. 위치를 바꾸려면 코드 폴더에 `roughcut_home.txt`를 만들고 경로 한 줄을 적습니다.
3. 코드 폴더에 `anthropic_key.txt`를 만들고 API 키 한 줄을 넣습니다. 없으면 구성 없이 침묵만 잘라냅니다.
4. `rules.podcast.example.md`를 복사해 `rules.md`로 저장하고 프로그램 정보와 팀 기준을 적습니다.

설정 파일 (모두 코드 폴더, 한 줄짜리 메모장 파일):

| 파일 | 내용 | 기본값 |
|---|---|---|
| `anthropic_key.txt` | API 키 | 없음. 없으면 규칙 플래너 |
| `rules.md` | 팀 편집 규칙 | `rules.podcast.example.md` |
| `profile.txt` | `podcast` 또는 `skit` | `podcast` |
| `whisper_model.txt` | `small`, `medium`, `large-v3` | `medium` |
| `roughcut_home.txt` | 무거운 파일 저장 위치 | `D:\roughcut` |

## 트랜스크립트를 직접 넣기 (권장)

긴 녹화를 CPU Whisper로 돌리면 오래 걸립니다. 팀의 subtitle-core 같은 도구로 만든 자막이 있으면 영상과 같은 폴더에 같은 이름으로 두십시오. `run.bat`이 Whisper를 건너뜁니다.

| 파일 | 내용 |
|---|---|
| `영상.srt` 또는 `영상.vtt` | 자막. `이름: 대사` 형식이면 화자로 읽음 |
| `영상.whisperx.json` | WhisperX 원본 JSON. 단어 타이밍과 화자 유지 |

## 터미널 사용

```bash
roughcut run 녹화.mp4 -o out --language ko --rules rules.md --target-minutes 45
roughcut plan out/outline.md -o out_v2          # 고친 구성안으로 타임라인만 다시
roughcut plan out/analysis.json -o out_v3       # 새 구성안
roughcut run 녹화.mp4 --transcriber srt --transcript 녹화.srt -o out
```

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--profile` | `podcast` | `skit`은 대본 촬영본(테이크 선별) |
| `--target-minutes` | 없음 | 완성본 목표 길이. 느슨한 목표 |
| `--effort` | `high` | 구성 품질과 비용. `medium`이면 저렴 |
| `--merge-gap` | podcast 1.0, skit 0.6 | 이보다 짧은 쉼은 자르지 않고 이어 붙임 (초) |
| `--scenes` | podcast 끔, skit 켬 | 화면 전환 감지. 긴 영상에서 느림 |

## 하는 일과 하지 않는 일

하는 일: 녹음 전후 잡담과 재시작 제거, "빼주세요" 요청 제거, 주제별 섹션 구성과 제목, 콜드 오픈 제안, 곁가지와 반복 제거, 뺀 구간의 이유 기록, 쇼츠 후보 표시.

하지 않는 일:
- 화면을 보지 않습니다. 구성은 대사만 보고 짭니다.
- 멀티캠 싱크를 하지 않습니다. 카메라가 여러 대면 프리미어에서 싱크한 마스터 1개나 대표 카메라 1개를 넣으십시오.
- 화자 분리는 외부 자막(subtitle-core, WhisperX)에 화자가 있을 때만 됩니다. 없으면 Claude가 문맥으로 추정합니다.
- 자막, 컬러, 오디오 믹스, B롤은 사람이 합니다.

Claude는 트랜스크립트의 **번호**로만 구간을 고릅니다. 시간을 지어내지 않습니다. 모든 컷은 실제 발화 경계에서 시작하고 끝납니다.

## GPU (선택)

기본은 CPU입니다. NVIDIA GPU가 있어도 CUDA 12용 cuBLAS와 cuDNN DLL이 없으면 `cublas64_12.dll is not found` 오류가 납니다. DLL을 설치한 뒤 `ROUGHCUT_WHISPER_DEVICE=cuda`를 지정합니다. https://github.com/SYSTRAN/faster-whisper#gpu

## 검증

```bash
pip install -e ".[dev]"
pytest
```

ffmpeg로 합성 영상을 만들어 전체 흐름을 네트워크 없이 돌립니다. Claude 호출은 스텁입니다.

## 한계

- 트랜스크립트 품질이 구성 품질을 결정합니다. 고유명사 오인식은 subtitle-core 검토 화면에서 고친 SRT를 쓰는 것이 가장 확실합니다.
- `.prproj`는 만들지 않습니다. 비공개 포맷입니다.
- OTIO 가져오기는 프리미어 26.0 이상. 그 아래는 `roughcut.xml`.
