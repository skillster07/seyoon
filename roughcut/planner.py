"""Analysis -> 구성안 (Outline) -> cut list (Plan).

Every planner produces an Outline first; the cut list is always derived from it by
outline.outline_to_plan. That keeps one editable artifact (outline.md) in the loop.

Planners:
  rules   no model call. One section covering all speech; filler-only utterances dropped.
  claude  Claude reads the whole numbered transcript plus the team rules and writes the 구성안:
          sections in playback order, each a list of contiguous segment-id spans, ids to drop
          inside spans, big excluded chunks with reasons, and short-form clip candidates.

Profiles tune the prompt and cut defaults:
  podcast long-form conversation; keep questions with answers, remove pre-show, off-record,
          tangents; optional cold open from a later highlight.
  skit    scripted takes; keep one best take of each repeated line, script order.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from pydantic import BaseModel, Field

from .models import Analysis, Cut, Plan, Segment
from .outline import Outline, OutlineSection, Span, is_filler_only, outline_to_plan

DEFAULT_MODEL = "claude-opus-5-5"

PROFILE_DEFAULTS = {
    # merge_gap: neighbouring kept utterances closer than this stay one clip (no jump cut).
    "podcast": {"pad_before": 0.15, "pad_after": 0.30, "merge_gap": 1.0},
    "skit": {"pad_before": 0.15, "pad_after": 0.25, "merge_gap": 0.6},
}


@dataclass
class PlanOptions:
    pad_before: float = 0.15
    pad_after: float = 0.25
    merge_gap: float = 0.6
    target_minutes: float | None = None
    rules_text: str = ""
    model: str = DEFAULT_MODEL
    effort: str = "high"
    profile: str = "podcast"


# --------------------------------------------------------------------------- cutting


def segments_to_cuts(segments: list[Segment], analysis: Analysis, opts: PlanOptions, section: str = "") -> list[Cut]:
    """Pad, clamp to media bounds, keep handles inside the source shot, merge close neighbours."""
    durations = {m.path: m.duration for m in analysis.media}
    shots_by_src: dict[str, list[tuple[float, float]]] = {}
    for sh in analysis.shots:
        shots_by_src.setdefault(sh.source, []).append((sh.start, sh.end))

    cuts: list[Cut] = []
    for seg in segments:
        start = max(0.0, seg.start - opts.pad_before)
        end = min(durations.get(seg.source, seg.end + opts.pad_after), seg.end + opts.pad_after)
        # A handle that crosses a hard cut in the source shows up as a flash frame.
        for s0, s1 in shots_by_src.get(seg.source, []):
            if s0 <= seg.start < s1:
                start = max(start, s0)
                end = min(end, s1) if seg.end <= s1 else end
                break
        label = seg.text if len(seg.text) <= 40 else seg.text[:37] + "..."
        cut = Cut(source=seg.source, start=start, end=end, label=label, reason=seg.text, section=section, segment_ids=[seg.id])
        prev = cuts[-1] if cuts else None
        # Merge when this utterance follows the previous clip in source time and the gap is short
        # (a negative gap means overlapping speech, which also merges).
        if (
            prev is not None
            and prev.source == cut.source
            and cut.start >= prev.start
            and cut.start - prev.end <= opts.merge_gap
        ):
            prev.end = max(prev.end, cut.end)
            prev.segment_ids.extend(cut.segment_ids)
            prev.reason = (prev.reason + " / " + seg.text)[:300]
        else:
            cuts.append(cut)
    return cuts


# Backwards-compatible name used by older callers/tests.
_segments_to_cuts = segments_to_cuts


# --------------------------------------------------------------------------- rules planner


def outline_rules(analysis: Analysis, opts: PlanOptions) -> Outline:
    """One section per source file covering all speech; filler-only utterances dropped."""
    secs: list[OutlineSection] = []
    by_src: dict[str, list[Segment]] = {}
    for s in sorted(analysis.segments, key=lambda s: (s.source, s.start)):
        by_src.setdefault(s.source, []).append(s)
    for src, segs in by_src.items():
        title = "전체" if len(by_src) == 1 else f"전체: {Path(src).name}"
        secs.append(
            OutlineSection(
                title=title,
                purpose="말이 있는 구간 전부",
                spans=[Span(segs[0].id, segs[-1].id)],
                drop_ids=[s.id for s in segs if is_filler_only(s.text)],
            )
        )
    return Outline(
        sections=secs,
        summary="규칙 플래너: 침묵과 필러만 제거. 구성은 하지 않음.",
        planner="rules",
    )


def plan_rules(analysis: Analysis, opts: PlanOptions) -> Plan:
    return outline_to_plan(outline_rules(analysis, opts), analysis, opts)


# --------------------------------------------------------------------------- claude planner


class SpanModel(BaseModel):
    from_id: int = Field(description="First segment id of the span (inclusive).")
    to_id: int = Field(description="Last segment id of the span (inclusive).")
    note: str = Field(description="Very short note on what this span contains.")


class SectionModel(BaseModel):
    title: str = Field(description="Short section title, shown as a timeline marker.")
    purpose: str = Field(description="Role in the episode, e.g. cold open, guest intro, topic, closing.")
    spans: list[SpanModel] = Field(description="Contiguous id ranges to play, in playback order.")
    drop_ids: list[int] = Field(description="Segment ids inside the spans to cut out.")
    rationale: str = Field(description="One sentence: why this section, in this place.")


class OutlineModel(BaseModel):
    summary: str = Field(description="Two or three sentences: what the episode is and how the edit is structured.")
    title_ideas: list[str] = Field(description="Three episode title ideas.")
    sections: list[SectionModel] = Field(description="Sections of the edited episode in playback order.")
    excluded: list[SpanModel] = Field(description="Large spans deliberately left out, each with the reason in note.")
    clip_candidates: list[SpanModel] = Field(description="Self-contained moments that work as standalone short clips.")


_COMMON_RULES = """Output rules:
- Refer to the recording only through segment ids. Never invent timestamps.
- A span is an inclusive range of consecutive segment ids. Prefer few long spans over many short ones.
- Use drop_ids only for ids inside that section's spans.
- Write titles, notes, rationale and summary in the language of the transcript.
- If a target duration is given, treat it as a soft target and state in the summary how far off you are.
- The team's rules file overrides these defaults wherever they conflict.
"""

PODCAST_PROMPT = (
    """You are the producer of a podcast, preparing the 구성안 (edit structure) and first assembly
for a human editor. You receive the full numbered transcript of a raw recording session.

Decide what the edited episode is and how it flows:
- Remove pre-show chatter, mic checks, setup, restarts, post-show talk, and anything a speaker asks
  to keep off the record (e.g. "이건 빼주세요", "방송에선 자르고"). Off-record requests are a hard rule.
- Group the conversation into topic sections with short titles. Keep source order inside a section.
- Never separate a question from its answer, and never cut in the middle of an answer or a story.
- Remove tangents that go nowhere, repeated explanations of the same point, false starts,
  and long stretches of agreement noises. Keep laughter and reactions that carry the moment.
- You may open with a short cold open: a strong moment taken from later in the recording,
  placed as the first section. Only do this when there is a clear highlight; keep it under a minute.
- Moving a whole topic section to a different position is allowed when it clearly improves flow;
  say so in the rationale.
- List the large chunks you left out under excluded, with the reason, so the editor can audit you.
- Suggest 3 to 8 clip_candidates: self-contained 30 to 90 second moments that make sense alone.

"""
    + _COMMON_RULES
)

SKIT_PROMPT = (
    """You are an assistant editor preparing a rough cut of scripted footage for a human editor.
You receive the full numbered transcript of the raw takes.

- When the same line is spoken more than once it is a retake: keep the single best take
  (complete, no stumble, not broken by laughter unless the rules say otherwise). Prefer the last
  complete take. Put the others in drop_ids or leave them out of the spans.
- Remove slate, director cues, "다시 갈게요", crew talk and camera checks.
- Keep script order. Sections follow scene changes.
- Never cut a line in half.
- clip_candidates may be empty.

"""
    + _COMMON_RULES
)

PROMPTS = {"podcast": PODCAST_PROMPT, "skit": SKIT_PROMPT}


def _fmt_ts(sec: float) -> str:
    h, rem = divmod(int(sec), 3600)
    m, s = divmod(rem, 60)
    return f"{h:02d}:{m:02d}:{s:02d}"


def transcript_for_model(analysis: Analysis) -> str:
    """Compact numbered transcript: one line per segment. Far fewer tokens than JSON."""
    lines = []
    multi = len({s.source for s in analysis.segments}) > 1
    for s in sorted(analysis.segments, key=lambda s: (s.source, s.start)):
        src = f"{Path(s.source).name} " if multi else ""
        spk = f"{s.speaker}: " if s.speaker else ""
        lines.append(f"#{s.id} [{src}{_fmt_ts(s.start)} +{s.end - s.start:.1f}s] {spk}{s.text}")
    return "\n".join(lines)


def outline_claude(analysis: Analysis, opts: PlanOptions) -> tuple[Outline, str]:
    """Returns (outline, usage note)."""
    import anthropic

    if not analysis.segments:
        raise ValueError("No transcript segments; the Claude planner needs speech to work with.")

    parts = []
    if opts.rules_text.strip():
        parts.append("## Team rules\n" + opts.rules_text.strip())
    if opts.target_minutes:
        parts.append(f"## Target duration\nAbout {opts.target_minutes:g} minutes.")
    total = sum(m.duration for m in analysis.media)
    parts.append(
        "## Recording\n"
        + "\n".join(f"- {m.name}: {_fmt_ts(m.duration)}" for m in analysis.media)
        + f"\nTotal {_fmt_ts(total)}. Speaker labels, when present, come from automatic diarization and may be wrong."
    )
    parts.append("## Transcript\n" + transcript_for_model(analysis))
    parts.append("Write the 구성안.")

    client = anthropic.Anthropic()
    # Streaming: long recordings mean long inputs and a long thinking phase; a non-streamed
    # request would risk the HTTP timeout. fallbacks="default" re-runs a request that the
    # primary model's safety classifier declines on Anthropic's recommended fallback model.
    with client.beta.messages.stream(
        model=opts.model,
        max_tokens=64000,
        betas=["server-side-fallback-2026-07-01"],
        fallbacks="default",
        system=PROMPTS.get(opts.profile, PODCAST_PROMPT),
        output_config={"effort": opts.effort},
        messages=[{"role": "user", "content": "\n\n".join(parts)}],
        output_format=OutlineModel,
    ) as stream:
        msg = stream.get_final_message()

    if msg.stop_reason == "refusal":
        detail = getattr(msg, "stop_details", None)
        raise RuntimeError(f"Claude declined the request: {getattr(detail, 'explanation', '') or 'no detail'}")
    if msg.stop_reason == "max_tokens":
        raise RuntimeError("Claude ran out of output tokens before finishing the 구성안. Try --effort medium.")
    parsed: OutlineModel | None = msg.parsed_output
    if parsed is None:
        raise RuntimeError("Claude returned no structured 구성안.")

    def sp(x: SpanModel) -> Span:
        return Span(from_id=x.from_id, to_id=x.to_id, note=x.note)

    outline = Outline(
        sections=[
            OutlineSection(
                title=s.title,
                purpose=s.purpose,
                spans=[sp(x) for x in s.spans],
                drop_ids=list(s.drop_ids),
                rationale=s.rationale,
            )
            for s in parsed.sections
        ],
        summary=parsed.summary,
        title_ideas=list(parsed.title_ideas),
        excluded=[sp(x) for x in parsed.excluded],
        clips=[sp(x) for x in parsed.clip_candidates],
        planner=f"claude:{getattr(msg, 'model', opts.model)}",
    )
    u = msg.usage
    usage = f"model={getattr(msg, 'model', opts.model)} input_tokens={u.input_tokens} output_tokens={u.output_tokens}"
    return outline, usage


def plan_claude(analysis: Analysis, opts: PlanOptions) -> Plan:
    outline, usage = outline_claude(analysis, opts)
    plan = outline_to_plan(outline, analysis, opts)
    plan.summary += f"\n({usage})"
    return plan


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
