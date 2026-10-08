"""ffprobe / ffmpeg helpers: metadata, audio extraction, silence detection, preview render."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from fractions import Fraction
from pathlib import Path

from .models import Cut, MediaInfo, Silence


class ToolMissing(RuntimeError):
    pass


def _require(tool: str) -> str:
    path = shutil.which(tool)
    if not path:
        raise ToolMissing(f"'{tool}' not found on PATH. Install ffmpeg (https://ffmpeg.org/).")
    return path


def probe(path: str | Path) -> MediaInfo:
    ffprobe = _require("ffprobe")
    out = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-print_format",
            "json",
            "-show_format",
            "-show_streams",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    data = json.loads(out)
    streams = data.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
    if video is None:
        raise ValueError(f"No video stream in {path}")

    fps_str = video.get("avg_frame_rate") or video.get("r_frame_rate") or "30/1"
    fps = float(Fraction(fps_str)) if fps_str not in ("0/0", "0") else 30.0

    duration = float(data.get("format", {}).get("duration") or video.get("duration") or 0.0)
    tc = (video.get("tags") or {}).get("timecode") or (data.get("format", {}).get("tags") or {}).get(
        "timecode"
    )
    return MediaInfo(
        path=str(Path(path).resolve()),
        duration=duration,
        fps=fps,
        width=int(video.get("width", 0)),
        height=int(video.get("height", 0)),
        has_audio=audio is not None,
        audio_sample_rate=int(audio["sample_rate"]) if audio and audio.get("sample_rate") else None,
        timecode_start=tc,
    )


def extract_audio(src: str | Path, dst: str | Path, sample_rate: int = 16000) -> Path:
    """Mono 16 kHz WAV, the input every speech model expects."""
    ffmpeg = _require("ffmpeg")
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-v",
            "error",
            "-i",
            str(src),
            "-vn",
            "-ac",
            "1",
            "-ar",
            str(sample_rate),
            "-c:a",
            "pcm_s16le",
            str(dst),
        ],
        check=True,
    )
    return dst


_SILENCE_START = re.compile(r"silence_start:\s*([0-9.]+)")
_SILENCE_END = re.compile(r"silence_end:\s*([0-9.]+)")


def detect_silences(
    src: str | Path, source_key: str, noise_db: float = -35.0, min_duration: float = 0.5
) -> list[Silence]:
    """Run ffmpeg's silencedetect filter and parse its log output."""
    ffmpeg = _require("ffmpeg")
    proc = subprocess.run(
        [
            ffmpeg,
            "-v",
            "info",
            "-nostats",
            "-i",
            str(src),
            "-af",
            f"silencedetect=noise={noise_db}dB:d={min_duration}",
            "-f",
            "null",
            "-",
        ],
        capture_output=True,
        text=True,
    )
    log = proc.stderr
    starts = [float(m.group(1)) for m in _SILENCE_START.finditer(log)]
    ends = [float(m.group(1)) for m in _SILENCE_END.finditer(log)]
    silences: list[Silence] = []
    for i, s in enumerate(starts):
        e = ends[i] if i < len(ends) else None
        if e is None:
            continue  # trailing silence without end is the tail; handled by duration
        silences.append(Silence(source=source_key, start=s, end=e))
    return silences


def render_preview(cuts: list[Cut], dst: str | Path, height: int = 540) -> Path:
    """Quick low-res MP4 of the cut list, for sanity checks before opening the NLE."""
    ffmpeg = _require("ffmpeg")
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)

    sources: list[str] = []
    for c in cuts:
        if c.source not in sources:
            sources.append(c.source)

    args = [ffmpeg, "-y", "-v", "error"]
    for s in sources:
        args += ["-i", s]

    parts = []
    concat_inputs = ""
    for i, c in enumerate(cuts):
        idx = sources.index(c.source)
        parts.append(
            f"[{idx}:v]trim=start={c.start:.3f}:end={c.end:.3f},setpts=PTS-STARTPTS,"
            f"scale=-2:{height}[v{i}]"
        )
        parts.append(f"[{idx}:a]atrim=start={c.start:.3f}:end={c.end:.3f},asetpts=PTS-STARTPTS[a{i}]")
        concat_inputs += f"[v{i}][a{i}]"
    parts.append(f"{concat_inputs}concat=n={len(cuts)}:v=1:a=1[vout][aout]")

    args += [
        "-filter_complex",
        ";".join(parts),
        "-map",
        "[vout]",
        "-map",
        "[aout]",
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "28",
        "-c:a",
        "aac",
        "-b:a",
        "96k",
        str(dst),
    ]
    subprocess.run(args, check=True)
    return dst
