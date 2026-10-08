"""Human-readable report next to the timeline files."""

from __future__ import annotations

from pathlib import Path

from .models import Analysis, Plan


def _fmt(sec: float) -> str:
    m, s = divmod(max(0.0, sec), 60)
    return f"{int(m):02d}:{s:05.2f}"


def write_report(analysis: Analysis, plan: Plan, out_dir: Path, outputs: dict[str, Path]) -> Path:
    total_src = sum(m.duration for m in analysis.media)
    lines = [
        "# Rough cut report",
        "",
        f"- Planner: `{plan.planner}`",
        f"- Transcriber: `{analysis.transcriber}`" + (f" (language={analysis.language})" if analysis.language else ""),
        f"- Source: {len(analysis.media)} file(s), {_fmt(total_src)} total",
        f"- Output: {len(plan.cuts)} clips, {_fmt(plan.total_duration)} ({plan.total_duration / total_src * 100 if total_src else 0:.0f}% of source)",
        f"- Segments: {len(analysis.segments)}  Shots: {len(analysis.shots)}  Silences: {len(analysis.silences)}",
        "",
        "## Files",
    ]
    for k, v in outputs.items():
        lines.append(f"- {k}: `{v.name}`")
    lines += ["", "## Summary", "", plan.summary or "(none)", ""]
    if plan.dropped:
        lines += ["## Dropped", ""] + [f"- {d}" for d in plan.dropped] + [""]

    lines += ["## Cut list", "", "| # | Section | Source | In | Out | Dur | Reason |", "|---|---|---|---|---|---|---|"]
    for i, c in enumerate(plan.cuts, 1):
        reason = c.reason.replace("|", "/").replace("\n", " ")
        if len(reason) > 80:
            reason = reason[:77] + "..."
        lines.append(
            f"| {i} | {c.section} | {Path(c.source).name} | {_fmt(c.start)} | {_fmt(c.end)} | {c.duration:.1f}s | {reason} |"
        )
    lines.append("")

    path = out_dir / "report.md"
    path.write_text("\n".join(lines), encoding="utf-8")
    return path
