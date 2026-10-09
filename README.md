# roughcut

원본 영상을 넣으면 프리미어 프로에서 바로 열리는 러프컷 타임라인(`.otio`, FCP 7 XML)이 나오는 CLI.

1단계 도구입니다. 사람이 쓰는 UI는 없고, 편집기를 대체하지 않습니다. 트랜스크립트와 샷 리스트를 뽑고, 팀 규칙에 따라 남길 구간을 고르고, 그 결과를 편집기가 읽는 파일로 씁니다. 마무리는 프리미어에서 합니다.

```
원본 .mp4/.mov ─▶ ffprobe/ffmpeg ─▶ 트랜스크립션 ─▶ 샷 감지 ─▶ 플래너 ─▶ roughcut.otio
                                   (whisper)     (PySceneDetect)  (rules | claude)   roughcut.xml
                                                                                     report.md
```

## Windows 빠른 설치 (터미널 없이)

요구 사항: Python 3.10 이상과 ffmpeg가 설치되어 있어야 함. 확인은 PowerShell에서 `python --version`, `ffmpeg -version`.

1. 저장소를 받습니다. `git clone https://github.com/skillster07/seyoon.git` 후 브랜치 `claude/beautiful-cori-6bbj15`로 전환. 이미 받았다면 폴더에서 `git pull`.
2. `setup.bat`을 더블클릭합니다. 가상환경을 만들고 패키지를 설치합니다. 몇 분 걸립니다. 마지막에 `roughcut 0.1.0`과 `DONE`이 보여야 합니다.
3. Claude 플래너를 쓰려면 같은 폴더에 `anthropic_key.txt` 파일을 만들고 API 키 한 줄만 넣습니다. 없으면 규칙 플래너로 돌아갑니다.
4. 영상 파일 하나를 `run.bat` 위로 드래그합니다. 영상 옆에 `영상이름_roughcut` 폴더가 생기고 자동으로 열립니다.

`run.bat` 기본값: 한국어, Whisper `medium` 모델, 미리보기 생성, 규칙 파일은 `rules.md`가 있으면 그것, 없으면 `rules.example.md`. 더 정확한 트랜스크립션이 필요하면 PowerShell에서 `$env:WHISPER_MODEL="medium"`을 설정한 뒤 실행합니다.

## 설치 (터미널)

요구 사항: Python 3.10 이상, ffmpeg와 ffprobe가 PATH에 있어야 함.

```bash
python -m venv .venv && source .venv/bin/activate
pip install -e ".[whisper]"        # faster-whisper 로컬 트랜스크립션 포함
# 또는 GPU가 있고 화자 분리가 필요하면
pip install -e ".[whisperx]"
```

Claude 플래너를 쓰려면 Anthropic API 키가 필요합니다.

```bash
export ANTHROPIC_API_KEY=sk-ant-...
```

키가 없으면 `--planner auto`는 규칙 기반 플래너로 떨어집니다. 규칙 플래너는 말이 있는 구간만 남기고 침묵을 버립니다. 구성은 하지 않습니다.

## 사용

```bash
# 가장 단순한 형태. 폴더 안의 영상 전부를 하나의 러프컷으로.
roughcut run ./footage -o ./out --language ko --rules rules.example.md

# 트랜스크립션을 다시 하지 않고 플래너만 다시 돌리기 (규칙 파일을 고친 뒤)
roughcut plan ./out/analysis.json -o ./out_v2 --rules rules.md --target-minutes 8

# API 없이 동작 확인
roughcut run clip.mp4 -o ./out --planner rules --preview
```

출력 폴더:

| 파일 | 내용 |
|---|---|
| `roughcut.otio` | 프리미어 26.0 이상에서 File, Import로 열기 |
| `roughcut.xml` | FCP 7 XML. 구버전 프리미어, 파이널컷 7 호환 툴용 |
| `report.md` | 컷 리스트, 섹션, 버린 이유 |
| `plan.json` | 플래너 결정. 사람이 고쳐서 `plan` 명령에 다시 넣을 수 있음 |
| `analysis.json` | 트랜스크립트, 샷, 침묵. 재사용 가능 |
| `preview.mp4` | `--preview` 지정 시 저해상 미리보기 |

주요 옵션:

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--transcriber` | `faster-whisper` | `faster-whisper`, `whisperx`, `json` |
| `--whisper-model` | `large-v3` | run.bat 기본값은 `medium`. 느리면 `small` |
| `--language` | 자동 | `ko` 권장. 자동 감지는 짧은 클립에서 틀림 |
| `--planner` | `auto` | `claude`, `rules` |
| `--model` | `claude-opus-5-5` | |
| `--effort` | `high` | `low`에서 `max` |
| `--target-minutes` | 없음 | Claude 플래너의 목표 길이 (느슨한 목표) |
| `--pad-before` / `--pad-after` | 0.15 / 0.25 | 발화 앞뒤 핸들(초) |
| `--merge-gap` | 0.6 | 이보다 가까운 구간은 한 클립으로 |

## 플래너가 하는 일과 하지 않는 일

Claude 플래너는 트랜스크립트의 **세그먼트 번호**만 고릅니다. 시간을 직접 쓰지 않습니다. 모든 컷은 실제 발화 경계에서 시작하고 끝납니다. 핸들을 더할 때 소스의 하드컷을 넘어가면 샷 경계에서 멈춥니다.

하는 일: 반복 테이크 중 하나 선택, 현장 대화 제거, 섹션 구성과 제목, 버린 이유 기록.

하지 않는 일: B롤 배치, 자막, 컬러, 오디오 믹스, 멀티캠 싱크, 프레임 단위 트리밍. 이것은 사람이 프리미어에서 합니다.

## 팀 규칙 파일

`rules.example.md`를 복사해서 팀이 실제로 반복하는 결정을 적으십시오. 프롬프트에 그대로 들어갑니다. 규칙이 구체적일수록 결과가 당신 팀의 편집에 가까워집니다. 완성본을 프리미어에서 OTIO로 다시 내보내 `plan.json`과 비교하면, 뒤집힌 결정이 다음 규칙입니다.

## 검증

```bash
pip install -e ".[dev]"
pytest
```

테스트는 ffmpeg로 합성 영상을 만들어 전체 파이프라인을 네트워크 없이 돌립니다. Claude 호출은 스텁으로 대체됩니다.

## 외부 트랜스크립트 사용 (subtitle-core 등)

이미 더 좋은 트랜스크립션 도구가 있으면 Whisper를 건너뛸 수 있습니다. 영상과 같은 폴더에 같은 이름으로 두면 `run.bat`이 자동으로 집어 씁니다.

| 파일 | 내용 | 비고 |
|---|---|---|
| `영상.srt` 또는 `영상.vtt` | 자막 큐 | 큐 단위 타이밍. 러프컷에는 충분 |
| `영상.whisperx.json` | WhisperX 원본 JSON | 단어 단위 타이밍 유지 |

터미널에서는 `--transcriber srt --transcript 영상.srt` 또는 `--transcriber json --transcript 영상.whisperx.json`입니다. SRT의 `이름: 대사` 접두와 VTT의 `<v 이름>` 태그는 화자로 읽습니다.

## GPU로 트랜스크립션 (선택)

기본은 CPU입니다. NVIDIA GPU가 있어도 CUDA 12용 cuBLAS와 cuDNN DLL이 없으면 `cublas64_12.dll is not found` 오류가 납니다. GPU를 쓰려면 faster-whisper 문서의 Windows 안내대로 DLL을 설치한 뒤 환경 변수를 지정합니다. https://github.com/SYSTRAN/faster-whisper#gpu

```powershell
$env:ROUGHCUT_WHISPER_DEVICE = "cuda"
```

CPU에서는 `small` 모델이 영상 길이와 비슷한 시간, `medium`은 그 두세 배가 걸립니다. [Inference]

## 한계 (현재 버전)

- 트랜스크립션 품질이 전부를 결정합니다. 한국어 정확도는 팀 영상으로 직접 확인해야 합니다.
- `.prproj`는 생성하지 않습니다. 비공개 포맷입니다. OTIO와 FCP XML만 씁니다.
- 멀티캠 싱크는 없습니다. 파일 여러 개를 넣으면 순서대로 한 타임라인에 올립니다.
- OTIO 가져오기는 프리미어 26.0 이상에서만 됩니다. 그 아래는 `roughcut.xml`을 쓰십시오.
