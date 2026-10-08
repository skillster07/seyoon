"""Shot boundary detection with PySceneDetect."""

from __future__ import annotations

from pathlib import Path

from .models import Shot


def detect_shots(video: str | Path, source_key: str, threshold: float = 27.0, min_shot_seconds: float = 0.5) -> list[Shot]:
    from scenedetect import ContentDetector, detect

    scene_list = detect(str(video), ContentDetector(threshold=threshold), show_progress=False)
    shots: list[Shot] = []
    for start, end in scene_list:
        s, e = start.seconds, end.seconds
        if e - s < min_shot_seconds and shots:
            shots[-1].end = e  # merge micro-shots into the previous one
            continue
        shots.append(Shot(source=source_key, start=s, end=e))
    return shots
