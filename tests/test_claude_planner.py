"""Claude planner mapping logic, with the API call replaced by a stub (no network, no key)."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

from roughcut.models import Analysis, MediaInfo, Segment, Shot
from roughcut.planner import PlanOptions, PlanResponse, PlanSection, plan_claude


def _analysis(src: str = "/tmp/fake.mp4") -> Analysis:
    media = MediaInfo(path=src, duration=60.0, fps=29.97, width=1920, height=1080, has_audio=True)
    segs = [
        Segment(id=0, source=src, start=1.0, end=3.0, text="안녕하세요"),
        Segment(id=1, source=src, start=3.2, end=6.0, text="오늘은 러프컷 이야기"),
        Segment(id=2, source=src, start=8.0, end=9.0, text="다시 갈게요"),
        Segment(id=3, source=src, start=10.0, end=14.0, text="오늘은 러프컷 이야기입니다"),
        Segment(id=4, source=src, start=20.0, end=25.0, text="첫째, 분석"),
        Segment(id=5, source=src, start=30.0, end=36.0, text="둘째, 결정"),
    ]
    shots = [Shot(source=src, start=0.0, end=15.0), Shot(source=src, start=15.0, end=60.0)]
    return Analysis(media=[media], segments=segs, shots=shots, silences=[], transcriber="json")


class _FakeMessages:
    def __init__(self, parsed: PlanResponse):
        self._parsed = parsed
        self.last_kwargs = None

    def parse(self, **kwargs):
        self.last_kwargs = kwargs
        return SimpleNamespace(
            stop_reason="end_turn",
            parsed_output=self._parsed,
            usage=SimpleNamespace(input_tokens=1234, output_tokens=56),
        )


def test_claude_planner_maps_segment_ids_to_cuts(monkeypatch: pytest.MonkeyPatch):
    fake = _FakeMessages(
        PlanResponse(
            sections=[
                PlanSection(title="인트로", segment_ids=[0, 3], rationale="best take of the intro line"),
                PlanSection(title="본론", segment_ids=[4, 5, 99], rationale="two points"),
            ],
            dropped=["segment 1: stumbled take", "segment 2: crew talk"],
            summary="Intro then two points.",
        )
    )
    import anthropic

    monkeypatch.setattr(anthropic, "Anthropic", lambda: SimpleNamespace(messages=fake))

    a = _analysis()
    plan = plan_claude(a, PlanOptions(rules_text="문장 중간에서 자르지 않는다.", target_minutes=1.0, model="claude-opus-5-5"))

    assert plan.planner == "claude:claude-opus-5-5"
    assert plan.sections == ["인트로", "본론"]
    # segments 0 and 3 are 7 s apart -> two cuts; 4 and 5 are 5 s apart -> two cuts
    assert [c.segment_ids for c in plan.cuts] == [[0], [3], [4], [5]]
    assert plan.cuts[0].section == "인트로" and plan.cuts[2].section == "본론"
    # unknown id 99 is reported, not silently dropped
    assert any("99" in d for d in plan.dropped)
    assert "input_tokens=1234" in plan.summary

    kw = fake.last_kwargs
    assert kw["model"] == "claude-opus-5-5"
    assert kw["output_format"] is PlanResponse
    assert kw["output_config"] == {"effort": "high"}
    body = kw["messages"][0]["content"]
    assert "문장 중간에서 자르지 않는다" in body and "Target duration" in body and '"id": 5' in body


def test_claude_planner_refusal_raises(monkeypatch: pytest.MonkeyPatch):
    import anthropic

    class _Refusing:
        def parse(self, **kwargs):
            return SimpleNamespace(stop_reason="refusal", stop_details=SimpleNamespace(explanation="policy"), parsed_output=None)

    monkeypatch.setattr(anthropic, "Anthropic", lambda: SimpleNamespace(messages=_Refusing()))
    with pytest.raises(RuntimeError, match="declined"):
        plan_claude(_analysis(), PlanOptions())
