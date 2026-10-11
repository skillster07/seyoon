"""구성안 (outline): the editorial structure between analysis and the cut list.

An outline is an ordered list of sections. Each section holds spans: inclusive ranges of
transcript segment ids taken from the raw recording, plus ids to drop inside those spans.

It is written to outline.md in a format a non-developer can edit in Notepad:
reorder "## " sections, delete or move "- #12-#45" lines, add "- 빼기: #20" lines.
`parse_outline_md` reads that file back, and `outline_to_plan` turns it into cuts without
any model call, so editing the 구성안 and rebuilding the timeline is free and deterministic.
"""

from __future__ import annotations

import re
from dataclasses import asdict, dataclass, field
from typing import Any

from .models import Analysis, Cut, Plan, Segment

# Sections whose title starts with one of these are notes for the editor, not timeline content.
NON_TIMELINE_PREFIXES = ("제외", "쇼츠", "참고", "메모", "excluded", "clips", "notes")

# Korean filler-only utterances. Dropped inside spans; conservative on purpose.
_FILLERS = {"음", "어", "으", "으음", "음음", "어어", "그", "그으", "아", "저", "에", "흠", "엄", "uh", "um", "hmm"}


@dataclass
class Span:
    from_id: int
    to_id: int
    note: str = ""


@dataclass
class OutlineSection:
    title: str
    purpose: str = ""
    spans: list[Span] = field(default_factory=list)
    drop_ids: list[int] = field(default_factory=list)
    rationale: str = ""

    @property
    def on_timeline(self) -> bool:
        t = self.title.strip().lower()
        return not any(t.startswith(p) for p in NON_TIMELINE_PREFIXES)


@dataclass
class Outline:
    sections: list[OutlineSection]
    summary: str = ""
    title_ideas: list[str] = field(default_factory=list)
    excluded: list[Span] = field(default_factory=list)
    clips: list[Span] = field(default_factory=list)
    planner: str = ""
    notes: list[str] = field(default_factory=list)  # warnings from the planner or the parser

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Outline":
        def spans(xs):
            return [Span(**x) for x in xs or []]

        return cls(
            sections=[
                OutlineSection(
                    title=s["title"],
                    purpose=s.get("purpose", ""),
                    spans=spans(s.get("spans")),
                    drop_ids=list(s.get("drop_ids", [])),
                    rationale=s.get("rationale", ""),
                )
                for s in d["sections"]
            ],
            summary=d.get("summary", ""),
            title_ideas=list(d.get("title_ideas", [])),
            excluded=spans(d.get("excluded")),
            clips=spans(d.get("clips")),
            planner=d.get("planner", ""),
            notes=list(d.get("notes", [])),
        )


def is_filler_only(text: str) -> bool:
    words = re.findall(r"[\w가-힣]+", text.lower())
    return bool(words) and all(w in _FILLERS for w in words)


def _ordered_ids(analysis: Analysis) -> list[int]:
    return [s.id for s in sorted(analysis.segments, key=lambda s: (s.source, s.start))]


def expand_span(span: Span, analysis: Analysis) -> list[Segment]:
    """Segments covered by an inclusive id range, in source order."""
    by_id = {s.id: s for s in analysis.segments}
    order = _ordered_ids(analysis)
    pos = {sid: i for i, sid in enumerate(order)}
    a, b = span.from_id, span.to_id
    if a not in pos or b not in pos:
        return []
    i, j = sorted((pos[a], pos[b]))
    return [by_id[sid] for sid in order[i : j + 1]]


def outline_to_plan(outline: Outline, analysis: Analysis, opts) -> Plan:
    """Deterministic: outline -> cuts. `opts` is planner.PlanOptions."""
    from .planner import segments_to_cuts  # local import: planner imports this module

    known = {s.id for s in analysis.segments}
    cuts: list[Cut] = []
    sections: list[str] = []
    notes = list(outline.notes)
    filler_dropped = 0

    for sec in outline.sections:
        if not sec.on_timeline:
            continue
        drop = set(sec.drop_ids)
        bad = sorted(d for d in drop if d not in known)
        if bad:
            notes.append(f"'{sec.title}': 없는 번호를 빼기 목록에서 무시함 {bad}")
        sec_cuts: list[Cut] = []
        for sp in sec.spans:
            segs = expand_span(sp, analysis)
            if not segs:
                notes.append(f"'{sec.title}': 구간 #{sp.from_id}-#{sp.to_id}를 찾을 수 없어 무시함")
                continue
            kept = []
            for s in segs:
                if s.id in drop:
                    continue
                if is_filler_only(s.text):
                    filler_dropped += 1
                    continue
                kept.append(s)
            # Cut each span on its own so merging never bridges two spans.
            sec_cuts.extend(segments_to_cuts(kept, analysis, opts, section=sec.title))
        if sec_cuts:
            sections.append(sec.title)
            cuts.extend(sec_cuts)

    if filler_dropped:
        notes.append(f"필러만 있는 발화 {filler_dropped}개 자동 제거")
    total_src = sum(m.duration for m in analysis.media)
    kept_total = sum(c.duration for c in cuts)
    summary = outline.summary or ""
    summary_tail = f"원본 {_fmt(total_src)} 중 {_fmt(kept_total)} 사용"
    return Plan(
        cuts=cuts,
        sections=sections,
        summary=(summary + "\n" + summary_tail).strip(),
        dropped=[f"{_fmt_span(sp, analysis)} {sp.note}".strip() for sp in outline.excluded] + notes,
        planner=outline.planner,
    )


# --------------------------------------------------------------------------- markdown


def _fmt(sec: float) -> str:
    sec = max(0.0, sec)
    h, rem = divmod(int(round(sec)), 3600)
    m, s = divmod(rem, 60)
    return f"{h:02d}:{m:02d}:{s:02d}"


def _fmt_span(sp: Span, analysis: Analysis) -> str:
    segs = expand_span(sp, analysis)
    ids = f"#{sp.from_id}" if sp.from_id == sp.to_id else f"#{sp.from_id}-#{sp.to_id}"
    if not segs:
        return ids
    return f"{ids} [{_fmt(segs[0].start)}-{_fmt(segs[-1].end)}]"


def _first_words(sp: Span, analysis: Analysis, n: int = 40) -> str:
    segs = [s for s in expand_span(sp, analysis) if not is_filler_only(s.text)]
    if not segs:
        return ""
    t = segs[0].text
    return t if len(t) <= n else t[: n - 1] + "…"


def _mmss(sec: float) -> str:
    m, s = divmod(int(round(sec)), 60)
    return f"{m}분 {s:02d}초"


EDIT_HELP = """<!--
편집 방법 (메모장으로 고친 뒤 이 파일을 replan.bat 위로 드래그하면 API 호출 없이 타임라인을 다시 만듭니다)
- "## " 로 시작하는 줄이 섹션입니다. 섹션 순서를 바꾸면 타임라인 순서가 바뀝니다. 제목은 마음대로 고쳐도 됩니다.
- "- #12-#45" 로 시작하는 줄이 넣을 구간입니다. 줄을 지우면 빠지고, 다른 섹션 아래로 옮기면 그 섹션으로 갑니다.
  번호 뒤의 [시간]과 글은 참고용이라 고치지 않아도 됩니다. "- #30" 처럼 한 번호만 써도 됩니다.
- "- 빼기: #20, #33" 줄의 번호는 그 섹션 구간 안에서 잘라냅니다.
- 제목이 "제외", "쇼츠", "참고", "메모" 로 시작하는 섹션은 타임라인에 들어가지 않습니다.
- 번호와 대사는 같은 폴더의 transcript.txt 에서 찾을 수 있습니다.
-->"""


def render_outline_md(outline: Outline, analysis: Analysis, plan: Plan | None = None) -> str:
    total_src = sum(m.duration for m in analysis.media)
    lines = ["# 구성안", "", EDIT_HELP, ""]
    if outline.summary:
        lines += [f"**요약**: {outline.summary}", ""]
    if outline.title_ideas:
        lines += ["**제목 후보**: " + " / ".join(outline.title_ideas), ""]
    if plan is not None:
        lines += [f"**예상 길이**: {_fmt(plan.total_duration)} (원본 {_fmt(total_src)})", ""]
    if outline.planner:
        lines += [f"**작성**: {outline.planner}", ""]

    sec_len: dict[str, float] = {}
    if plan is not None:
        for c in plan.cuts:
            sec_len[c.section] = sec_len.get(c.section, 0.0) + c.duration

    n = 0
    for sec in outline.sections:
        if sec.on_timeline:
            n += 1
            lines.append(f"## {n}. {sec.title}")
        else:
            lines.append(f"## {sec.title}")
        meta = " / ".join(
            x
            for x in (
                f"길이: {_mmss(sec_len[sec.title])}" if sec.title in sec_len else "",
                f"목적: {sec.purpose}" if sec.purpose else "",
                f"이유: {sec.rationale}" if sec.rationale else "",
            )
            if x
        )
        if meta:
            lines.append(f"> {meta}")
        for sp in sec.spans:
            note = sp.note or _first_words(sp, analysis)
            lines.append(f"- {_fmt_span(sp, analysis)} {note}".rstrip())
        if sec.drop_ids:
            lines.append("- 빼기: " + ", ".join(f"#{i}" for i in sorted(set(sec.drop_ids))))
        lines.append("")

    if outline.excluded:
        lines.append("## 제외")
        lines.append("> 타임라인에 넣지 않은 큰 구간. 살리려면 위 섹션 아래로 줄을 옮기십시오.")
        for sp in outline.excluded:
            lines.append(f"- {_fmt_span(sp, analysis)} {sp.note}".rstrip())
        lines.append("")
    if outline.clips:
        lines.append("## 쇼츠 후보")
        lines.append("> 단독으로 잘라 써도 되는 구간. 타임라인에는 들어가지 않습니다.")
        for sp in outline.clips:
            lines.append(f"- {_fmt_span(sp, analysis)} {sp.note}".rstrip())
        lines.append("")
    if outline.notes:
        lines.append("## 메모")
        lines += [f"- {x}" for x in outline.notes]
        lines.append("")
    return "\n".join(lines)


_H2 = re.compile(r"^##\s+(.*\S)\s*$")
_NUMBERED = re.compile(r"^\d+[.)]\s*")
_SPAN = re.compile(r"^\s*[-*]\s*#(\d+)(?:\s*[-–~]\s*#?(\d+))?(.*)$")
_DROP = re.compile(r"^\s*[-*]\s*빼기\s*[:：]\s*(.*)$")
_QUOTE = re.compile(r"^\s*>\s*(.*)$")
_SUMMARY = re.compile(r"^\*\*요약\*\*\s*[:：]\s*(.*)$")
_TITLES = re.compile(r"^\*\*제목 후보\*\*\s*[:：]\s*(.*)$")
_PLANNER = re.compile(r"^\*\*작성\*\*\s*[:：]\s*(.*)$")


def parse_outline_md(text: str) -> Outline:
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    sections: list[OutlineSection] = []
    excluded: list[Span] = []
    clips: list[Span] = []
    summary, titles, planner = "", [], ""
    current: OutlineSection | None = None
    kind = "timeline"

    for raw in text.splitlines():
        line = raw.rstrip()
        if m := _H2.match(line):
            title = _NUMBERED.sub("", m.group(1)).strip()
            low = title.lower()
            if low.startswith(("제외", "excluded")):
                kind, current = "excluded", None
            elif low.startswith(("쇼츠", "clips")):
                kind, current = "clips", None
            elif low.startswith(("참고", "메모", "notes")):
                kind, current = "ignore", None
            else:
                kind = "timeline"
                current = OutlineSection(title=title)
                sections.append(current)
            continue
        if current is None and kind == "timeline":
            if m := _SUMMARY.match(line):
                summary = m.group(1).strip()
            elif m := _TITLES.match(line):
                titles = [t.strip() for t in m.group(1).split("/") if t.strip()]
            elif m := _PLANNER.match(line):
                planner = m.group(1).strip() + " (편집됨)" if "(편집됨)" not in m.group(1) else m.group(1).strip()
            continue
        if m := _DROP.match(line):
            if current is not None:
                current.drop_ids += [int(x) for x in re.findall(r"#?(\d+)", m.group(1))]
            continue
        if m := _SPAN.match(line):
            a = int(m.group(1))
            b = int(m.group(2)) if m.group(2) else a
            note = re.sub(r"^\s*\[[^\]]*\]\s*", "", m.group(3) or "").strip()
            sp = Span(from_id=a, to_id=b, note=note)
            if kind == "timeline" and current is not None:
                current.spans.append(sp)
            elif kind == "excluded":
                excluded.append(sp)
            elif kind == "clips":
                clips.append(sp)
            continue
        if (m := _QUOTE.match(line)) and current is not None and kind == "timeline":
            for part in m.group(1).split(" / "):
                if part.startswith("목적:"):
                    current.purpose = part[3:].strip()
                elif part.startswith("이유:"):
                    current.rationale = part[3:].strip()

    return Outline(
        sections=sections,
        summary=summary,
        title_ideas=titles,
        excluded=excluded,
        clips=clips,
        planner=planner or "outline.md (편집됨)",
    )


def render_transcript_txt(analysis: Analysis) -> str:
    """Numbered transcript for humans editing outline.md."""
    lines = []
    for s in sorted(analysis.segments, key=lambda s: (s.source, s.start)):
        spk = f"{s.speaker}: " if s.speaker else ""
        lines.append(f"#{s.id}\t[{_fmt(s.start)}]\t{spk}{s.text}")
    return "\n".join(lines) + "\n"
