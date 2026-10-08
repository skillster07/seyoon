"""Write the plan as OpenTimelineIO (.otio) and Final Cut Pro 7 XML (.xml).

Premiere Pro 26.0+ imports .otio directly (File > Import). Older versions use the FCP 7 XML.
"""

from __future__ import annotations

from pathlib import Path

import opentimelineio as otio

from .models import Cut, MediaInfo, Plan


def _rt(seconds: float, rate: float) -> otio.opentime.RationalTime:
    return otio.opentime.RationalTime(round(seconds * rate), rate)


def _range(start: float, end: float, rate: float) -> otio.opentime.TimeRange:
    return otio.opentime.TimeRange(start_time=_rt(start, rate), duration=_rt(end - start, rate))


def build_timeline(plan: Plan, media: list[MediaInfo], name: str = "roughcut") -> otio.schema.Timeline:
    by_path = {m.path: m for m in media}
    rate = media[0].fps if media else 30.0

    tl = otio.schema.Timeline(name=name)
    tl.global_start_time = otio.opentime.RationalTime(0, rate)
    video = otio.schema.Track(name="V1", kind=otio.schema.TrackKind.Video)
    audio = otio.schema.Track(name="A1", kind=otio.schema.TrackKind.Audio)
    tl.tracks.append(video)
    tl.tracks.append(audio)

    refs: dict[str, otio.schema.ExternalReference] = {}
    for m in media:
        refs[m.path] = otio.schema.ExternalReference(
            target_url=Path(m.path).as_uri(),
            available_range=_range(0.0, m.duration, m.fps),
        )

    playhead = 0.0
    last_section = None
    for i, cut in enumerate(plan.cuts):
        m = by_path.get(cut.source)
        src_rate = m.fps if m else rate
        label = cut.label or f"{Path(cut.source).stem} {i + 1:03d}"

        for track in (video, audio):
            clip = otio.schema.Clip(
                name=label,
                media_reference=refs[cut.source].clone() if cut.source in refs else otio.schema.MissingReference(),
                source_range=_range(cut.start, cut.end, src_rate),
            )
            clip.metadata["roughcut"] = {
                "reason": cut.reason,
                "section": cut.section,
                "segment_ids": list(cut.segment_ids),
            }
            if track is video and cut.reason:
                clip.markers.append(
                    otio.schema.Marker(
                        name=cut.reason[:120],
                        marked_range=otio.opentime.TimeRange(_rt(cut.start, src_rate), _rt(0, src_rate)),
                        color=otio.schema.MarkerColor.GREEN,
                    )
                )
            track.append(clip)

        # Section boundary markers on the timeline track, where editors look first.
        if cut.section and cut.section != last_section:
            video.markers.append(
                otio.schema.Marker(
                    name=cut.section,
                    marked_range=otio.opentime.TimeRange(_rt(playhead, rate), _rt(0, rate)),
                    color=otio.schema.MarkerColor.CYAN,
                )
            )
            last_section = cut.section
        playhead += cut.duration

    tl.metadata["roughcut"] = {
        "planner": plan.planner,
        "summary": plan.summary,
        "sections": list(plan.sections),
    }
    return tl


def write_outputs(tl: otio.schema.Timeline, out_dir: Path, stem: str = "roughcut") -> dict[str, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    otio_path = out_dir / f"{stem}.otio"
    otio.adapters.write_to_file(tl, str(otio_path))
    written = {"otio": otio_path}

    xml_path = out_dir / f"{stem}.xml"
    try:
        otio.adapters.write_to_file(tl, str(xml_path), adapter_name="fcp_xml")
        written["fcp_xml"] = xml_path
    except Exception as e:  # adapter missing or unsupported feature; .otio is still valid
        written["fcp_xml_error"] = Path(str(e))
    return written


def cuts_summary(cuts: list[Cut]) -> str:
    total = sum(c.duration for c in cuts)
    return f"{len(cuts)} clips, {total / 60:.1f} min"
