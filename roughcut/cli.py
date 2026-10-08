"""roughcut command line.

  roughcut run  <media...> -o out/ [--rules rules.md] [--planner claude|rules] [--transcriber faster-whisper|whisperx|json]
  roughcut plan <out/analysis.json> -o out/ [--rules rules.md] [--planner ...]   # re-plan without re-analysing
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import asdict
from pathlib import Path

from . import __version__
from .media import ToolMissing, detect_silences, extract_audio, probe, render_preview
from .models import Analysis, plan_to_dict
from .planner import PlanOptions, has_anthropic_credentials, load_rules, plan_claude, plan_rules
from .report import write_report
from .scenes import detect_shots
from .timeline import build_timeline, write_outputs
from .transcribe import load_transcript_json, transcribe

VIDEO_EXTS = {".mp4", ".mov", ".mxf", ".mkv", ".avi", ".m4v", ".mts", ".m2ts", ".webm"}


def _log(msg: str) -> None:
    print(f"[roughcut] {msg}", file=sys.stderr, flush=True)


def _collect_media(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        pp = Path(p)
        if pp.is_dir():
            out.extend(sorted(x for x in pp.iterdir() if x.suffix.lower() in VIDEO_EXTS))
        elif pp.exists():
            out.append(pp)
        else:
            raise FileNotFoundError(p)
    if not out:
        raise SystemExit("No video files found.")
    return out


def _plan_options(args: argparse.Namespace) -> PlanOptions:
    return PlanOptions(
        pad_before=args.pad_before,
        pad_after=args.pad_after,
        merge_gap=args.merge_gap,
        target_minutes=args.target_minutes,
        rules_text=load_rules(args.rules),
        model=args.model,
        effort=args.effort,
    )


def _choose_planner(name: str) -> str:
    if name == "auto":
        return "claude" if has_anthropic_credentials() else "rules"
    return name


def _do_plan_and_write(analysis: Analysis, args: argparse.Namespace, out_dir: Path) -> int:
    opts = _plan_options(args)
    planner = _choose_planner(args.planner)
    _log(f"planning with '{planner}'")
    t0 = time.time()
    plan = plan_claude(analysis, opts) if planner == "claude" else plan_rules(analysis, opts)
    _log(f"plan: {len(plan.cuts)} cuts, {plan.total_duration:.0f}s ({time.time() - t0:.1f}s)")
    if not plan.cuts:
        _log("plan has no cuts; nothing to write")
        return 2

    (out_dir / "plan.json").write_text(json.dumps(plan_to_dict(plan), ensure_ascii=False, indent=2), encoding="utf-8")

    tl = build_timeline(plan, analysis.media, name=args.name or out_dir.name)
    outputs = write_outputs(tl, out_dir, stem=args.name or "roughcut")
    outputs["plan"] = out_dir / "plan.json"
    outputs["analysis"] = out_dir / "analysis.json"

    if args.preview:
        _log("rendering preview")
        try:
            outputs["preview"] = render_preview(plan.cuts, out_dir / "preview.mp4")
        except Exception as e:  # preview is a convenience; never fail the run on it
            _log(f"preview failed: {e}")

    report = write_report(analysis, plan, out_dir, outputs)
    _log(f"wrote {outputs['otio'].name}" + (", " + outputs["fcp_xml"].name if "fcp_xml" in outputs else "") + f", {report.name}")
    for k, v in outputs.items():
        print(f"{k}: {v}")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    media_paths = _collect_media(args.media)
    out_dir = Path(args.out)
    work = out_dir / "work"
    work.mkdir(parents=True, exist_ok=True)

    media = []
    for p in media_paths:
        info = probe(p)
        _log(f"probe {info.name}: {info.duration:.1f}s {info.width}x{info.height} @ {info.fps:.3f}fps audio={info.has_audio}")
        media.append(info)

    segments = []
    silences = []
    shots = []
    for info in media:
        key = info.path
        if info.has_audio:
            wav = extract_audio(info.path, work / (Path(info.path).stem + ".wav"))
            if args.transcriber == "json":
                if not args.transcript:
                    raise SystemExit("--transcriber json requires --transcript <file>")
                segs = load_transcript_json(Path(args.transcript), key)
            else:
                _log(f"transcribing {info.name} with {args.transcriber} ({args.whisper_model})")
                segs = transcribe(args.transcriber, wav, key, args.language, args.whisper_model)
            # Re-number globally so ids stay unique across several source files.
            for s in segs:
                s.id = len(segments)
                segments.append(s)
            _log(f"  {len(segs)} segments")
            silences.extend(detect_silences(wav, key, noise_db=args.silence_db, min_duration=args.min_silence))
        else:
            _log(f"{info.name} has no audio track; skipping transcription")
        if not args.no_scenes:
            sh = detect_shots(info.path, key, threshold=args.scene_threshold)
            _log(f"  {len(sh)} shots")
            shots.extend(sh)

    analysis = Analysis(
        media=media,
        segments=segments,
        shots=shots,
        silences=silences,
        language=args.language,
        transcriber=args.transcriber,
    )
    (out_dir / "analysis.json").write_text(json.dumps(analysis.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    return _do_plan_and_write(analysis, args, out_dir)


def cmd_plan(args: argparse.Namespace) -> int:
    analysis = Analysis.from_dict(json.loads(Path(args.analysis).read_text(encoding="utf-8")))
    out_dir = Path(args.out or Path(args.analysis).parent)
    out_dir.mkdir(parents=True, exist_ok=True)
    if not (out_dir / "analysis.json").exists():
        (out_dir / "analysis.json").write_text(json.dumps(analysis.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    return _do_plan_and_write(analysis, args, out_dir)


def _add_plan_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("-o", "--out", help="output directory")
    p.add_argument("--name", help="timeline / file stem (default: output dir name)")
    p.add_argument("--rules", help="markdown file with the team's editing rules (fed to the Claude planner)")
    p.add_argument("--planner", choices=["auto", "claude", "rules"], default="auto", help="auto = claude if credentials exist, else rules")
    p.add_argument("--model", default="claude-opus-5-5")
    p.add_argument("--effort", choices=["low", "medium", "high", "xhigh", "max"], default="high")
    p.add_argument("--target-minutes", type=float, help="soft target length for the Claude planner")
    p.add_argument("--pad-before", type=float, default=0.15, help="seconds of handle before speech")
    p.add_argument("--pad-after", type=float, default=0.25, help="seconds of handle after speech")
    p.add_argument("--merge-gap", type=float, default=0.6, help="join kept segments closer than this (s)")
    p.add_argument("--preview", action="store_true", help="also render a low-res preview.mp4 of the cut")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="roughcut", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"roughcut {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    run = sub.add_parser("run", help="analyse footage and write a rough cut")
    run.add_argument("media", nargs="+", help="video files or directories")
    run.add_argument("--transcriber", choices=["faster-whisper", "whisperx", "json"], default="faster-whisper")
    run.add_argument("--transcript", help="existing transcript JSON (with --transcriber json)")
    run.add_argument("--whisper-model", default="large-v3", help="whisper model size (large-v3, medium, small...)")
    run.add_argument("--language", default=None, help="ISO code, e.g. ko. Default: auto-detect")
    run.add_argument("--no-scenes", action="store_true", help="skip shot detection")
    run.add_argument("--scene-threshold", type=float, default=27.0)
    run.add_argument("--silence-db", type=float, default=-35.0)
    run.add_argument("--min-silence", type=float, default=0.5)
    _add_plan_args(run)
    run.set_defaults(func=cmd_run)

    plan = sub.add_parser("plan", help="re-plan from an existing analysis.json (no re-transcription)")
    plan.add_argument("analysis", help="path to analysis.json")
    _add_plan_args(plan)
    plan.set_defaults(func=cmd_plan)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.cmd == "run" and not args.out:
        args.out = str(Path(args.media[0]).with_suffix("")) + "_roughcut"
    try:
        return args.func(args)
    except ToolMissing as e:
        _log(str(e))
        return 127
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
