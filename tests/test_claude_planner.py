"""Claude 구성안 planner with the API call replaced by a stub (no network, no key)."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

from roughcut.models import Analysis, MediaInfo, Segment, Shot
from roughcut.outline import outline_to_plan
from roughcut.planner import OutlineModel, PlanOptions, SectionModel, SpanModel, outline_claude, plan_claude


def _analysis(src: str = "/tmp/pod.mp4") -> Analysis:
    media = MediaInfo(path=src, duration=600.0, fps=29.97, width=1920, height=1080, has_audio=True)
    texts = [
        (0, 5, "마이크 테스트 하나 둘"),  # 0 pre-show
        (6, 12, "자 시작할게요"),  # 1
        (20, 30, "안녕하세요 오늘 게스트는"),  # 2 intro
        (31, 40, "반갑습니다"),  # 3
        (41, 42, "음"),  # 4 filler-only
        (43, 60, "첫 질문입니다 어떻게 시작하셨어요"),  # 5 topic 1
        (61, 90, "처음엔 진짜 망했다고 생각했죠"),  # 6 highlight
        (91, 100, "이건 방송에선 빼주세요"),  # 7 off-record
        (101, 130, "두 번째 주제로 가보죠"),  # 8 topic 2
        (131, 150, "오늘 고맙습니다"),  # 9 closing
    ]
    segs = [Segment(id=i, source=src, start=a, end=b, text=t, speaker="S1" if i % 2 else "S2") for i, (a, b, t) in enumerate(texts)]
    return Analysis(media=[media], segments=segs, shots=[Shot(source=src, start=0, end=600)], silences=[], transcriber="srt")


class _Stream:
    def __init__(self, msg):
        self._msg = msg

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def get_final_message(self):
        return self._msg


class _FakeBetaMessages:
    def __init__(self, msg):
        self._msg = msg
        self.kwargs = None

    def stream(self, **kwargs):
        self.kwargs = kwargs
        return _Stream(self._msg)


def _install(monkeypatch, msg):
    import anthropic

    fake = _FakeBetaMessages(msg)
    monkeypatch.setattr(anthropic, "Anthropic", lambda: SimpleNamespace(beta=SimpleNamespace(messages=fake)))
    return fake


def _ok(parsed: OutlineModel):
    return SimpleNamespace(
        stop_reason="end_turn",
        parsed_output=parsed,
        model="claude-opus-5-5",
        usage=SimpleNamespace(input_tokens=5000, output_tokens=800),
    )


PARSED = OutlineModel(
    summary="게스트 인터뷰. 콜드 오픈 후 두 주제.",
    title_ideas=["망했다고 생각했던 순간", "B", "C"],
    sections=[
        SectionModel(title="콜드 오픈", purpose="cold open", spans=[SpanModel(from_id=6, to_id=6, note="망했다")], drop_ids=[], rationale="훅"),
        SectionModel(title="인사", purpose="intro", spans=[SpanModel(from_id=2, to_id=3, note="")], drop_ids=[], rationale=""),
        SectionModel(
            title="시작 이야기",
            purpose="topic",
            spans=[SpanModel(from_id=4, to_id=9, note="")],
            drop_ids=[7, 999],
            rationale="오프더레코드 제외",
        ),
    ],
    excluded=[SpanModel(from_id=0, to_id=1, note="녹음 전 준비")],
    clip_candidates=[SpanModel(from_id=6, to_id=6, note="망했다는 고백")],
)


def test_outline_request_shape(monkeypatch: pytest.MonkeyPatch):
    fake = _install(monkeypatch, _ok(PARSED))
    outline, usage = outline_claude(_analysis(), PlanOptions(rules_text="오프닝은 짧게", target_minutes=3, profile="podcast"))
    kw = fake.kwargs
    assert kw["model"] == "claude-opus-5-5"
    assert kw["output_format"] is OutlineModel
    assert kw["fallbacks"] == "default" and kw["betas"] == ["server-side-fallback-2026-07-01"]
    assert kw["max_tokens"] >= 32000
    body = kw["messages"][0]["content"]
    assert "오프닝은 짧게" in body and "About 3 minutes" in body
    assert "#7 [00:01:31 +9.0s] S1: 이건 방송에선 빼주세요" in body
    assert "podcast" in kw["system"].lower()
    assert outline.sections[0].title == "콜드 오픈" and outline.clips[0].note == "망했다는 고백"
    assert "input_tokens=5000" in usage


def test_outline_to_plan_order_drops_and_fillers(monkeypatch: pytest.MonkeyPatch):
    _install(monkeypatch, _ok(PARSED))
    a = _analysis()
    plan = plan_claude(a, PlanOptions(merge_gap=1.0, profile="podcast"))
    # cold open first, even though it is later in the source
    assert plan.cuts[0].segment_ids == [6] and plan.cuts[0].section == "콜드 오픈"
    kept = [i for c in plan.cuts for i in c.segment_ids]
    assert 7 not in kept[1:]  # off-record dropped in the topic section
    assert 4 not in kept  # filler-only "음" dropped automatically
    assert 0 not in kept and 1 not in kept  # excluded pre-show
    assert plan.sections == ["콜드 오픈", "인사", "시작 이야기"]
    assert any("999" in d for d in plan.dropped)  # unknown id reported, not silently ignored
    assert any("필러" in d for d in plan.dropped)


@pytest.mark.parametrize(
    "stop_reason,match",
    [("refusal", "declined"), ("max_tokens", "ran out")],
)
def test_bad_stop_reasons_raise(monkeypatch: pytest.MonkeyPatch, stop_reason, match):
    msg = SimpleNamespace(stop_reason=stop_reason, stop_details=SimpleNamespace(explanation="x"), parsed_output=None)
    _install(monkeypatch, msg)
    with pytest.raises(RuntimeError, match=match):
        outline_claude(_analysis(), PlanOptions())


def test_skit_profile_uses_skit_prompt(monkeypatch: pytest.MonkeyPatch):
    fake = _install(monkeypatch, _ok(PARSED))
    outline_claude(_analysis(), PlanOptions(profile="skit"))
    assert "retake" in fake.kwargs["system"]
    # and the plan can still be derived deterministically from an outline alone
    outline, _ = outline_claude(_analysis(), PlanOptions(profile="skit"))
    assert outline_to_plan(outline, _analysis(), PlanOptions()).cuts
