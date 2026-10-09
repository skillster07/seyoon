"""Loaders for transcripts produced elsewhere: SRT/VTT cues and raw WhisperX JSON."""

from __future__ import annotations

import json
from pathlib import Path

from roughcut.transcribe import load_subtitle_file, load_transcript_json

SRT = """1
00:00:01,560 --> 00:00:04,760
지효: 아니 어 좀

2
00:00:05,560 --> 00:00:06,680
못내고
두 줄짜리

3
00:00:07,000 --> 00:00:07,000
빈 길이

"""

VTT = """WEBVTT

00:00:01.560 --> 00:00:04.760
<v 철모>집들이 온 거야?

00:00:05.000 --> 00:00:06.500
<i>집들이는</i> 무슨 집들이야
"""


def test_srt_cues_with_speaker_and_multiline(tmp_path: Path):
    p = tmp_path / "a.srt"
    p.write_text(SRT, encoding="utf-8")
    segs = load_subtitle_file(p, "/x.mp4")
    assert [s.id for s in segs] == [0, 1]  # zero-length cue dropped
    assert segs[0].speaker == "지효" and segs[0].text == "아니 어 좀"
    assert segs[0].start == 1.56 and segs[0].end == 4.76
    assert segs[1].text == "못내고 두 줄짜리" and segs[1].speaker is None


def test_vtt_voice_tag_and_markup(tmp_path: Path):
    p = tmp_path / "a.vtt"
    p.write_text(VTT, encoding="utf-8")
    segs = load_subtitle_file(p, "/x.mp4")
    assert segs[0].speaker == "철모" and segs[0].text == "집들이 온 거야?"
    assert segs[1].text == "집들이는 무슨 집들이야"


def test_raw_whisperx_json(tmp_path: Path):
    data = {
        "language": "ko",
        "segments": [
            {
                "start": 0.5,
                "end": 2.0,
                "text": " 첫 문장",
                "speaker": "SPEAKER_00",
                "words": [
                    {"word": "첫", "start": 0.5, "end": 0.9, "score": 0.9},
                    {"word": "문장", "start": 1.0, "end": 2.0, "score": 0.8},
                ],
            }
        ],
    }
    p = tmp_path / "a.whisperx.json"
    p.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    segs = load_transcript_json(p, "/x.mp4")
    assert segs[0].text == "첫 문장" and segs[0].speaker == "SPEAKER_00"
    assert [w.text for w in segs[0].words] == ["첫", "문장"]
    assert segs[0].words[1].confidence == 0.8
