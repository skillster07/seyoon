"""roughcut command line.

  roughcut run  <media...> -o out/ [--profile podcast|skit] [--rules rules.md] [--planner claude|rules]
                [--transcriber faster-whisper|whisperx|json|srt --transcript file]
  roughcut plan out/analysis.json -o out_v2/   # new 구성안 from the saved analysis (planner runs again)
  roughcut plan out/outline.md    -o out_v2/   # rebuild the timeline from an edited 구성안 (no model call)

Outputs in the output folder:
  outline.md       구성안. Edit it in Notepad, then run `plan outline.md` to rebuild the timeline.
  transcript.txt   numbered transcript, for finding ids while editing outline.md
  roughcut.otio    Premiere Pro 26+ (File > Import); roughcut.xml for older versions
  report.md        cut list with reasons
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from . import __version__
from .media import ToolMissing, detect_silences, extract_audio, probe, render_preview
from .models import Analysis, Plan, plan_to_dict
from .outline import Outline, outline_to_plan, parse_outline_md, render_outline_md, render_transcript_txt
from .planner import PROFILE_DEFAULTS, PlanOptions, has_anthropic_credentials, load_rules, outline_claude, outline_rules
from .report import write_report
from .scenes import detect_shots
from .timeline import build_timeline, write_outputs
from .transcribe import load_subtitle_file, load_transcript_json, transcribe

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
    d = PROFILE_DEFAULTS.get(args.profile, PROFILE_DEFAULTS["podcast"])
    return PlanOptions(
        pad_before=args.pad_before if args.pad_before is not None else d["pad_before"],
        pad_after=args.pad_after if args.pad_after is not None else d["pad_after"],
        merge_gap=args.merge_gap if args.merge_gap is not None else d["merge_gap"],
        target_minutes=args.target_minutes,
        rules_text=load_rules(args.rules),
        model=args.model,
        effort=args.effort,
        profile=args.profile,
    )


def _choose_planner(name: str) -> str:
    if name == "auto":
        return "claude" if has_anthropic_credentials() else "rules"
    return name


def _make_outline(analysis: Analysis, args: argparse.Namespace, opts: PlanOptions) -> tuple[Outline, str]:
    planner = _choose_planner(args.planner)
    _log(f"writing 구성안 with '{planner}' (profile={opts.profile})")
    t0 = time.time()
    if planner == "claude":
        outline, usage = outline_claude(analysis, opts)
    else:
        outline, usage = outline_rules(analysis, opts), ""
    _log(f"구성안: {sum(1 for s in outline.sections if s.on_timeline)} sections ({time.time() - t0:.1f}s)")
    return outline, usage


def _write_all(analysis: Analysis, outline: Outline, usage: str, args: argparse.Namespace, out_dir: Path) -> int:
    opts = _plan_options(args)
    plan: Plan = outline_to_plan(outline, analysis, opts)
    if usage:
        plan.summary += f"\n({usage})"
    _log(f"timeline: {len(plan.cuts)} clips, {plan.total_duration / 60:.1f} min")

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "outline.md").write_text(render_outline_md(outline, analysis, plan), encoding="utf-8")
    (out_dir / "outline.json").write_text(json.dumps(outline.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    (out_dir / "transcript.txt").write_text(render_transcript_txt(analysis), encoding="utf-8")
    if not (out_dir / "analysis.json").exists():
        (out_dir / "analysis.json").write_text(json.dumps(analysis.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")

    if not plan.cuts:
        _log("구성안에 타임라인에 넣을 구간이 없습니다. outline.md를 확인하십시오.")
        return 2

    (out_dir / "plan.json").write_text(json.dumps(plan_to_dict(plan), ensure_ascii=False, indent=2), encoding="utf-8")
    tl = build_timeline(plan, analysis.media, name=args.name or out_dir.name)
    outputs = write_outputs(tl, out_dir, stem=args.name or "roughcut")
    outputs["outline"] = out_dir / "outline.md"
    outputs["transcript"] = out_dir / "transcript.txt"
    outputs["plan"] = out_dir / "plan.json"
    outputs["analysis"] = out_dir / "analysis.json"

    if args.preview:
        _log("rendering preview")
        try:
            outputs["preview"] = render_preview(plan.cuts, out_dir / "preview.mp4")
        except Exception as e:  # preview is a convenience; never fail the run on it
            _log(f"preview failed: {e}")

    report = write_report(analysis, plan, out_dir, outputs)
    _log(f"wrote outline.md, {outputs['otio'].name}, {report.name}")
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
    if args.profile == "podcast" and len(media) > 1:
        _log(
            "주의: 파일이 여러 개입니다. 멀티캠 싱크는 하지 않고 순서대로 이어 붙입니다. "
            "멀티캠 녹화라면 프리미어에서 싱크한 마스터 1개 또는 대표 카메라 1개를 넣으십시오."
        )

    do_scenes = args.scenes if args.scenes is not None else (args.profile == "skit")
    segments = []
    silences = []
    shots = []
    for info in media:
        key = info.path
        if info.has_audio:
            wav = extract_audio(info.path, work / (Path(info.path).stem + ".wav"))
            if args.transcriber in ("json", "srt"):
                if not args.transcript:
                    raise SystemExit(f"--transcriber {args.transcriber} requires --transcript <file>")
                loader = load_transcript_json if args.transcriber == "json" else load_subtitle_file
                segs = loader(Path(args.transcript), key)
                _log(f"loaded transcript from {Path(args.transcript).name}")
            else:
                _log(f"transcribing {info.name} with {args.transcriber} ({args.whisper_model}); long recordings take a while")
                segs = transcribe(args.transcriber, wav, key, args.language, args.whisper_model)
            # Re-number globally so ids stay unique across several source files.
            for s in segs:
                s.id = len(segments)
                segments.append(s)
            _log(f"  {len(segs)} segments")
            silences.extend(detect_silences(wav, key, noise_db=args.silence_db, min_duration=args.min_silence))
        else:
            _log(f"{info.name} has no audio track; skipping transcription")
        if do_scenes:
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
    (out_dir / "transcript.txt").write_text(render_transcript_txt(analysis), encoding="utf-8")

    opts = _plan_options(args)
    outline, usage = _make_outline(analysis, args, opts)
    rc = _write_all(analysis, outline, usage, args, out_dir)
    if rc == 0 and not args.keep_work:
        # The extracted WAVs are only needed during analysis; analysis.json carries everything else.
        for wav in work.glob("*.wav"):
            try:
                wav.unlink()
            except OSError:
                pass
        try:
            work.rmdir()
        except OSError:
            pass
    return rc


def cmd_plan(args: argparse.Namespace) -> int:
    src = Path(args.input)
    if src.suffix.lower() == ".md":
        analysis_path = Path(args.analysis) if args.analysis else src.parent / "analysis.json"
        if not analysis_path.exists():
            raise SystemExit(f"analysis.json not found next to {src.name}; pass --analysis")
        analysis = Analysis.from_dict(json.loads(analysis_path.read_text(encoding="utf-8")))
        raw = src.read_bytes()
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:  # Notepad "ANSI" save on Korean Windows
            text = raw.decode("cp949")
        outline = parse_outline_md(text)
        _log(f"read edited 구성안 {src.name}: {sum(1 for s in outline.sections if s.on_timeline)} sections")
        usage = ""
    else:
        analysis = Analysis.from_dict(json.loads(src.read_text(encoding="utf-8")))
        outline, usage = _make_outline(analysis, args, _plan_options(args))
    out_dir = Path(args.out or src.parent)
    out_dir.mkdir(parents=True, exist_ok=True)
    if not (out_dir / "analysis.json").exists():
        (out_dir / "analysis.json").write_text(json.dumps(analysis.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    return _write_all(analysis, outline, usage, args, out_dir)


def _add_plan_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("-o", "--out", help="output directory")
    p.add_argument("--name", help="timeline / file stem (default: output dir name)")
    p.add_argument("--profile", choices=sorted(PROFILE_DEFAULTS), default="podcast", help="editing style (default podcast)")
    p.add_argument("--rules", help="markdown file with the team's editing rules (fed to the Claude planner)")
    p.add_argument("--planner", choices=["auto", "claude", "rules"], default="auto", help="auto = claude if credentials exist, else rules")
    p.add_argument("--model", default="claude-opus-5-5")
    p.add_argument("--effort", choices=["low", "medium", "high", "xhigh", "max"], default="high")
    p.add_argument("--target-minutes", type=float, help="soft target length for the Claude planner")
    p.add_argument("--pad-before", type=float, default=None, help="seconds of handle before speech (profile default)")
    p.add_argument("--pad-after", type=float, default=None, help="seconds of handle after speech (profile default)")
    p.add_argument("--merge-gap", type=float, default=None, help="join kept utterances closer than this, seconds (profile default)")
    p.add_argument("--preview", action="store_true", help="also render a low-res preview.mp4 of the cut")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="roughcut", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"roughcut {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    run = sub.add_parser("run", help="analyse footage, write a 구성안 and a rough cut")
    run.add_argument("media", nargs="+", help="video files or directories")
    run.add_argument("--transcriber", choices=["faster-whisper", "whisperx", "json", "srt"], default="faster-whisper")
    run.add_argument("--transcript", help="existing transcript: JSON (analysis/WhisperX/faster-whisper) or SRT/VTT")
    run.add_argument("--whisper-model", default="large-v3", help="whisper model size (large-v3, medium, small...)")
    run.add_argument("--language", default=None, help="ISO code, e.g. ko. Default: auto-detect")
    run.add_argument(
        "--scenes", dest="scenes", action="store_true", default=None, help="detect shot boundaries (default on for skit, off for podcast)"
    )
    run.add_argument("--no-scenes", dest="scenes", action="store_false", help="skip shot detection")
    run.add_argument("--keep-work", action="store_true", help="keep extracted WAVs in <out>/work after a successful run")
    run.add_argument("--scene-threshold", type=float, default=27.0)
    run.add_argument("--silence-db", type=float, default=-35.0)
    run.add_argument("--min-silence", type=float, default=0.5)
    _add_plan_args(run)
    run.set_defaults(func=cmd_run)

    plan = sub.add_parser("plan", help="new 구성안 from analysis.json, or rebuild the timeline from an edited outline.md")
    plan.add_argument("input", help="analysis.json (re-plan) or outline.md (rebuild from edited 구성안, no model call)")
    plan.add_argument("--analysis", help="analysis.json to use with an outline.md (default: next to it)")
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
