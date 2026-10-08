"""Offline tests: synthetic footage + hand-written transcript, rules planner, OTIO/FCP XML output."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import opentimelineio as otio
import pytest

from roughcut.cli import main
from roughcut.models import Analysis, MediaInfo, Segment
from roughcut.planner import PlanOptions, plan_rules
from roughcut.timeline import build_timeline

ffmpeg_missing = shutil.which("ffmpeg") is None


def _make_clip(path: Path, seconds: int = 12, fps: int = 25) -> None:
    """Colour bars that change every 4 s (so shot detection has something to find) plus tone/silence."""
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            f"testsrc=size=320x180:rate={fps}:duration={seconds}",
            "-f",
            "lavfi",
            "-i",
            f"sine=frequency=440:duration={seconds}",
            "-af",
            "volume='if(between(t,4,6),0,1)':eval=frame",
            "-vf",
            "hue='H=floor(t/4)*2':s=1",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            str(path),
        ],
        check=True,
    )


def _analysis(src: str) -> Analysis:
    media = MediaInfo(path=src, duration=12.0, fps=25.0, width=320, height=180, has_audio=True)
    segs = [
        Segment(id=0, source=src, start=0.5, end=2.0, text="안녕하세요 오늘 주제는"),
        Segment(id=1, source=src, start=2.1, end=3.8, text="영상 편집 자동화입니다"),
        Segment(id=2, source=src, start=6.5, end=8.0, text="다시 갈게요"),
        Segment(id=3, source=src, start=9.0, end=11.5, text="핵심은 러프컷을 빨리 만드는 것"),
    ]
    return Analysis(media=[media], segments=segs, shots=[], silences=[], transcriber="json")


def test_rules_planner_merges_close_segments_and_pads():
    a = _analysis("/tmp/fake.mp4")
    plan = plan_rules(a, PlanOptions(pad_before=0.1, pad_after=0.2, merge_gap=0.5))
    # segments 0 and 1 are 0.1 s apart -> merged; 2 and 3 stay separate
    assert len(plan.cuts) == 3
    first = plan.cuts[0]
    assert first.start == pytest.approx(0.4)
    assert first.end == pytest.approx(4.0)
    assert first.segment_ids == [0, 1]
    assert plan.cuts[-1].end <= 12.0


def test_build_timeline_frame_accurate():
    a = _analysis("/tmp/fake.mp4")
    plan = plan_rules(a, PlanOptions())
    tl = build_timeline(plan, a.media, name="t")
    video = [t for t in tl.tracks if t.kind == otio.schema.TrackKind.Video][0]
    audio = [t for t in tl.tracks if t.kind == otio.schema.TrackKind.Audio][0]
    assert len(video) == len(audio) == len(plan.cuts)
    for clip, cut in zip(video, plan.cuts):
        assert clip.source_range.start_time.rate == 25.0
        assert clip.source_range.start_time.value == round(cut.start * 25)
    assert video.markers[0].name == "Rough cut"
    # round-trips through the JSON serialiser
    assert otio.adapters.read_from_string(otio.adapters.write_to_string(tl)).name == "t"


@pytest.mark.skipif(ffmpeg_missing, reason="ffmpeg not installed")
def test_end_to_end_with_json_transcript(tmp_path: Path):
    clip = tmp_path / "take1.mp4"
    _make_clip(clip)
    transcript = tmp_path / "take1.json"
    transcript.write_text(
        json.dumps(
            {
                "segments": [
                    {"start": 0.5, "end": 2.0, "text": "첫 문장"},
                    {"start": 6.5, "end": 8.0, "text": "둘째 문장"},
                    {"start": 9.0, "end": 11.0, "text": "마지막 문장"},
                ]
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    out = tmp_path / "out"
    rc = main(
        [
            "run",
            str(clip),
            "-o",
            str(out),
            "--transcriber",
            "json",
            "--transcript",
            str(transcript),
            "--planner",
            "rules",
            "--preview",
        ]
    )
    assert rc == 0
    assert (out / "roughcut.otio").exists()
    assert (out / "roughcut.xml").exists()
    assert (out / "report.md").exists()
    assert (out / "preview.mp4").exists()

    analysis = json.loads((out / "analysis.json").read_text(encoding="utf-8"))
    assert len(analysis["segments"]) == 3
    assert len(analysis["shots"]) >= 2  # colour changes every 4 s
    assert any(s["start"] >= 3.5 and s["end"] <= 6.5 for s in analysis["silences"])

    tl = otio.adapters.read_from_file(str(out / "roughcut.otio"))
    video = [t for t in tl.tracks if t.kind == otio.schema.TrackKind.Video][0]
    assert len(video) == 3
    assert video[0].media_reference.target_url.startswith("file://")

    # re-plan from the saved analysis without re-running the analysis stage
    rc = main(["plan", str(out / "analysis.json"), "-o", str(tmp_path / "out2"), "--planner", "rules", "--name", "v2"])
    assert rc == 0
    assert (tmp_path / "out2" / "v2.otio").exists()
