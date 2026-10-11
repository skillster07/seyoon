"""outline.md: render, hand-edit, parse back."""

from __future__ import annotations

from roughcut.models import Analysis, MediaInfo, Segment
from roughcut.outline import Outline, OutlineSection, Span, outline_to_plan, parse_outline_md, render_outline_md
from roughcut.planner import PlanOptions


def _analysis() -> Analysis:
    src = "/tmp/pod.mp4"
    segs = [Segment(id=i, source=src, start=i * 10.0, end=i * 10.0 + 8.0, text=f"문장 {i}") for i in range(12)]
    return Analysis(media=[MediaInfo(path=src, duration=130.0, fps=30.0, width=1920, height=1080, has_audio=True)], segments=segs, shots=[], silences=[])


def _outline() -> Outline:
    return Outline(
        sections=[
            OutlineSection(title="오프닝", purpose="훅", spans=[Span(8, 8, "하이라이트")], rationale="강한 시작"),
            OutlineSection(title="본론", spans=[Span(2, 7)], drop_ids=[5]),
        ],
        summary="요약 문장",
        title_ideas=["제목A", "제목B"],
        excluded=[Span(0, 1, "녹음 전 잡담")],
        clips=[Span(3, 4, "쇼츠감")],
        planner="claude:test",
    )


def test_render_then_parse_roundtrip():
    a = _analysis()
    md = render_outline_md(_outline(), a, outline_to_plan(_outline(), a, PlanOptions()))
    assert "## 1. 오프닝" in md and "- 빼기: #5" in md and "## 제외" in md and "## 쇼츠 후보" in md
    assert "[00:01:20-00:01:28]" in md  # span #8 times
    back = parse_outline_md(md)
    assert [s.title for s in back.sections] == ["오프닝", "본론"]
    assert back.sections[0].purpose == "훅" and back.sections[0].rationale == "강한 시작"
    assert back.sections[1].spans[0].from_id == 2 and back.sections[1].spans[0].to_id == 7
    assert back.sections[1].drop_ids == [5]
    assert back.excluded[0].note == "녹음 전 잡담" and back.clips[0].from_id == 3
    assert back.summary == "요약 문장" and back.title_ideas == ["제목A", "제목B"]
    # the example lines inside the help comment must not be parsed as spans
    assert all(sp.from_id != 12 for s in back.sections for sp in s.spans)


def test_hand_edits_reorder_move_and_drop():
    a = _analysis()
    edited = """# 구성안
**요약**: 고친 버전

## 1. 본론 먼저
- #2-#4 [아무거나] 메모
- #9
- 빼기: #3

## 2. 오프닝은 뒤로
- #8 하이라이트

## 제외
- #0-#1
"""
    plan = outline_to_plan(parse_outline_md(edited), a, PlanOptions(merge_gap=0.5))
    ids = [i for c in plan.cuts for i in c.segment_ids]
    assert ids == [2, 4, 9, 8]
    assert plan.sections == ["본론 먼저", "오프닝은 뒤로"]


def test_reversed_range_and_missing_ids_are_reported():
    a = _analysis()
    o = Outline(sections=[OutlineSection(title="A", spans=[Span(4, 2), Span(50, 60)])])
    plan = outline_to_plan(o, a, PlanOptions(merge_gap=0.5))
    assert [i for c in plan.cuts for i in c.segment_ids] == [2, 3, 4]
    assert any("#50-#60" in d for d in plan.dropped)
