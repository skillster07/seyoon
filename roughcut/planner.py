"""Turn analysis into a cut list.

Two planners:
  rules   deterministic baseline: keep every spoken segment, drop silence, pad edges.
  claude  Claude reads the transcript + shot list + your rules file and picks/organises segments.

The Claude planner selects *segment ids*, never free timestamps, so every cut maps back to
real speech boundaries from the transcript.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

from pydantic import BaseModel, Field

from .models import Analysis, Cut, Plan, Segment

DEFAULT_MODEL = "claude-opus-5-5"


@dataclass
class PlanOptions:
    pad_before: float = 0.15  # seconds of handle before speech
    pad_after: float = 0.25  # seconds of handle after speech
    merge_gap: float = 0.6  # join neighbouring kept segments if the gap is shorter than this
    target_minutes: float | None = None  # soft target for the Claude planner
    rules_text: str = ""
    model: str = DEFAULT_MODEL
    effort: str = "high"


# --------------------------------------------------------------------------- rules planner


def _segments_to_cuts(
    segments: list[Segment], analysis: Analysis, opts: PlanOptions, section: str = "", reason_prefix: str = ""
) -> list[Cut]:
    """Pad, clamp to media bounds, snap to shot boundaries when a pad would cross one, merge."""
    durations = {m.path: m.duration for m in analysis.media}
    shots_by_src: dict[str, list[tuple[float, float]]] = {}
    for sh in analysis.shots:
        shots_by_src.setdefault(sh.source, []).append((sh.start, sh.end))

    cuts: list[Cut] = []
    for seg in segments:
        start = max(0.0, seg.start - opts.pad_before)
        end = min(durations.get(seg.source, seg.end + opts.pad_after), seg.end + opts.pad_after)
        # Don't let a handle cross a hard cut in the source: that reads as a flash frame.
        for s0, s1 in shots_by_src.get(seg.source, []):
            if s0 <= seg.start < s1:
                start = max(start, s0)
                end = min(end, s1) if seg.end <= s1 else end
                break
        label = seg.text if len(seg.text) <= 40 else seg.text[:37] + "..."
        reason = f"{reason_prefix}{seg.text}" if reason_prefix else seg.text
        cut = Cut(source=seg.source, start=start, end=end, label=label, reason=reason, section=section, segment_ids=[seg.id])
        if cuts and cuts[-1].source == cut.source and cut.start - cuts[-1].end <= opts.merge_gap and cuts[-1].section == section:
            prev = cuts[-1]
            prev.end = max(prev.end, cut.end)
            prev.segment_ids.extend(cut.segment_ids)
            prev.reason = (prev.reason + " / " + seg.text)[:300]
        else:
            cuts.append(cut)
    return cuts


def plan_rules(analysis: Analysis, opts: PlanOptions) -> Plan:
    segs = sorted(analysis.segments, key=lambda s: (s.source, s.start))
    cuts = _segments_to_cuts(segs, analysis, opts, section="Rough cut")
    total_src = sum(m.duration for m in analysis.media)
    kept = sum(c.duration for c in cuts)
    return Plan(
        cuts=cuts,
        sections=["Rough cut"],
        summary=f"Kept all speech ({kept:.0f}s of {total_src:.0f}s). Silence and non-speech dropped.",
        dropped=[f"{total_src - kept:.0f}s of silence / non-speech"],
        planner="rules",
    )


# --------------------------------------------------------------------------- claude planner


class PlanSection(BaseModel):
    title: str = Field(description="Short section title shown as a timeline marker.")
    segment_ids: list[int] = Field(description="Transcript segment ids to keep, in playback order.")
    rationale: str = Field(description="One sentence: why these segments, in this order.")


class PlanResponse(BaseModel):
    sections: list[PlanSection]
    dropped: list[str] = Field(description="Short notes on what was deliberately left out and why.")
    summary: str = Field(description="Two or three sentences describing the structure of the rough cut.")


SYSTEM_PROMPT = """You are an assistant editor preparing a rough cut for a human editor.

You receive a transcript (numbered segments with timings and optional speaker), a shot list,
and the team's editing rules. Your job is the first pass only: decide which spoken segments to
keep, group them into titled sections in playback order, and leave everything else out.

Rules of the job:
- Select by segment id only. Never invent timings.
- Keep complete thoughts. Do not cut a sentence in half by dropping the segment that finishes it.
- Remove repeated takes: when the same line is spoken more than once, keep the single best take
  (complete, no stumble, no laughter unless the rules say to keep it) and note the drop.
- Remove false starts, filler-only segments, and crew talk unless the rules say otherwise.
- Reordering is allowed when it serves the structure, but prefer source order inside a section.
- If a target duration is given, treat it as a soft target and say in the summary how far off you are.
- The team's rules file overrides these defaults wherever they conflict.
"""


def _segments_payload(analysis: Analysis) -> list[dict]:
    rows = []
    for s in analysis.segments:
        rows.append(
            {
                "id": s.id,
                "src": Path(s.source).name,
                "start": round(s.start, 2),
                "end": round(s.end, 2),
                "speaker": s.speaker,
                "text": s.text,
            }
        )
    return rows


def plan_claude(analysis: Analysis, opts: PlanOptions) -> Plan:
    import anthropic

    if not analysis.segments:
        raise ValueError("No transcript segments; the Claude planner needs speech to work with.")

    client = anthropic.Anthropic()

    user_parts = []
    if opts.rules_text.strip():
        user_parts.append("## Team editing rules\n" + opts.rules_text.strip())
    if opts.target_minutes:
        user_parts.append(f"## Target duration\nAbout {opts.target_minutes:g} minutes.")
    user_parts.append(
        "## Media\n"
        + "\n".join(f"- {m.name}: {m.duration:.0f}s, {m.width}x{m.height} @ {m.fps:.2f}fps" for m in analysis.media)
    )
    if analysis.shots:
        shots = [
            {"src": Path(sh.source).name, "start": round(sh.start, 2), "end": round(sh.end, 2)} for sh in analysis.shots
        ]
        user_parts.append("## Shot list (visual cuts in the source)\n" + json.dumps(shots, ensure_ascii=False))
    user_parts.append(
        "## Transcript segments\n" + json.dumps(_segments_payload(analysis), ensure_ascii=False, indent=None)
    )
    user_parts.append("Produce the rough cut plan.")

    response = client.messages.parse(
        model=opts.model,
        max_tokens=16000,
        system=SYSTEM_PROMPT,
        output_config={"effort": opts.effort},
        messages=[{"role": "user", "content": "\n\n".join(user_parts)}],
        output_format=PlanResponse,
    )
    if response.stop_reason == "refusal":
        detail = getattr(response, "stop_details", None)
        raise RuntimeError(f"Claude declined the request: {getattr(detail, 'explanation', '')}")
    parsed: PlanResponse = response.parsed_output  # type: ignore[assignment]

    by_id = {s.id: s for s in analysis.segments}
    cuts: list[Cut] = []
    sections: list[str] = []
    unknown: list[int] = []
    for sec in parsed.sections:
        segs = []
        for sid in sec.segment_ids:
            if sid in by_id:
                segs.append(by_id[sid])
            else:
                unknown.append(sid)
        if not segs:
            continue
        sections.append(sec.title)
        cuts.extend(_segments_to_cuts(segs, analysis, opts, section=sec.title))

    dropped = list(parsed.dropped)
    if unknown:
        dropped.append(f"Planner referenced unknown segment ids (ignored): {sorted(set(unknown))}")

    usage = response.usage
    summary = parsed.summary + f"\n(model={opts.model}, input_tokens={usage.input_tokens}, output_tokens={usage.output_tokens})"
    return Plan(cuts=cuts, sections=sections, summary=summary, dropped=dropped, planner=f"claude:{opts.model}")


def load_rules(path: str | Path | None) -> str:
    if not path:
        return ""
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"Rules file not found: {p}")
    return p.read_text(encoding="utf-8")


def has_anthropic_credentials() -> bool:
    if os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN"):
        return True
    return (Path.home() / ".config" / "anthropic").exists()
