"""Data model shared by every stage. All times are seconds (float) from source start."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class MediaInfo:
    path: str
    duration: float
    fps: float
    width: int
    height: int
    has_audio: bool
    audio_sample_rate: int | None = None
    timecode_start: str | None = None  # e.g. "01:00:00:00" if the container carries one

    @property
    def name(self) -> str:
        return Path(self.path).name


@dataclass
class Word:
    start: float
    end: float
    text: str
    confidence: float | None = None


@dataclass
class Segment:
    """One spoken utterance (a transcript line)."""

    id: int
    source: str  # MediaInfo.path
    start: float
    end: float
    text: str
    speaker: str | None = None
    words: list[Word] = field(default_factory=list)

    @property
    def duration(self) -> float:
        return self.end - self.start


@dataclass
class Shot:
    """A visually continuous span (between two hard cuts in the source)."""

    source: str
    start: float
    end: float


@dataclass
class Silence:
    source: str
    start: float
    end: float


@dataclass
class Cut:
    """One clip on the output timeline, in timeline order."""

    source: str
    start: float  # in point, source seconds
    end: float  # out point, source seconds
    label: str = ""  # short label shown as clip name
    reason: str = ""  # why this span was kept (goes into a clip marker)
    section: str = ""  # section title this cut belongs to
    segment_ids: list[int] = field(default_factory=list)

    @property
    def duration(self) -> float:
        return self.end - self.start


@dataclass
class Plan:
    cuts: list[Cut]
    sections: list[str]
    summary: str = ""
    dropped: list[str] = field(default_factory=list)  # human-readable notes on what was dropped
    planner: str = ""  # "rules" or "claude:<model>"

    @property
    def total_duration(self) -> float:
        return sum(c.duration for c in self.cuts)


@dataclass
class Analysis:
    """Everything produced before planning. Serialised to analysis.json for reuse."""

    media: list[MediaInfo]
    segments: list[Segment]
    shots: list[Shot]
    silences: list[Silence]
    language: str | None = None
    transcriber: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Analysis":
        return cls(
            media=[MediaInfo(**m) for m in d["media"]],
            segments=[
                Segment(**{**s, "words": [Word(**w) for w in s.get("words", [])]})
                for s in d["segments"]
            ],
            shots=[Shot(**s) for s in d.get("shots", [])],
            silences=[Silence(**s) for s in d.get("silences", [])],
            language=d.get("language"),
            transcriber=d.get("transcriber", ""),
        )


def plan_to_dict(plan: Plan) -> dict[str, Any]:
    return asdict(plan)


def plan_from_dict(d: dict[str, Any]) -> Plan:
    return Plan(
        cuts=[Cut(**c) for c in d["cuts"]],
        sections=list(d.get("sections", [])),
        summary=d.get("summary", ""),
        dropped=list(d.get("dropped", [])),
        planner=d.get("planner", ""),
    )
