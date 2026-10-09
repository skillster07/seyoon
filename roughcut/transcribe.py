"""Transcription backends. Each returns list[Segment] with word timings when available.

Backends:
  faster-whisper  local, CPU or GPU, word timestamps built in   (pip install roughcut[whisper])
  whisperx        local, GPU recommended, wav2vec2 alignment     (pip install roughcut[whisperx])
  json            load a transcript produced earlier: our analysis.json schema, raw WhisperX JSON
                  (segments[].words[] with word/start/end/score), or faster-whisper style
  srt             load an SRT / WebVTT file (cue-level timing, no words)
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Callable

from .models import Segment, Word

Transcriber = Callable[[Path, str, str | None], list[Segment]]


def _faster_whisper(audio: Path, source_key: str, language: str | None, model_size: str = "large-v3") -> list[Segment]:
    try:
        from faster_whisper import WhisperModel  # type: ignore
    except ImportError as e:  # pragma: no cover
        raise RuntimeError("faster-whisper is not installed: pip install 'roughcut[whisper]'") from e

    import os
    import sys

    # Default to CPU. GPU needs CUDA + cuBLAS + cuDNN DLLs that most machines don't have;
    # opt in with ROUGHCUT_WHISPER_DEVICE=cuda once they are installed.
    device = os.environ.get("ROUGHCUT_WHISPER_DEVICE", "cpu")
    compute = os.environ.get("ROUGHCUT_WHISPER_COMPUTE", "int8" if device == "cpu" else "float16")

    def _run(dev: str, comp: str):
        model = WhisperModel(model_size, device=dev, compute_type=comp)
        segments, _info = model.transcribe(
            str(audio),
            language=language,
            word_timestamps=True,
            vad_filter=True,
        )
        return list(segments)  # generator: force it here so GPU failures surface inside this call

    try:
        raw_segments = _run(device, compute)
    except (RuntimeError, OSError) as e:
        msg = str(e).lower()
        if device != "cpu" and any(k in msg for k in ("cublas", "cudnn", "cuda", "library")):
            print(f"[roughcut] GPU transcription failed ({e}); falling back to CPU int8", file=sys.stderr)
            raw_segments = _run("cpu", "int8")
        else:
            raise

    out: list[Segment] = []
    for i, s in enumerate(raw_segments):
        words = [
            Word(start=float(w.start), end=float(w.end), text=w.word.strip(), confidence=float(w.probability))
            for w in (s.words or [])
        ]
        text = s.text.strip()
        if not text:
            continue
        out.append(Segment(id=i, source=source_key, start=float(s.start), end=float(s.end), text=text, words=words))
    return out


def _whisperx(audio: Path, source_key: str, language: str | None, model_size: str = "large-v3") -> list[Segment]:
    try:
        import whisperx  # type: ignore
    except ImportError as e:  # pragma: no cover
        raise RuntimeError("whisperx is not installed: pip install 'roughcut[whisperx]'") from e

    import os

    device = os.environ.get("ROUGHCUT_WHISPER_DEVICE", "cuda")
    compute = os.environ.get("ROUGHCUT_WHISPER_COMPUTE", "float16" if device == "cuda" else "int8")
    model = whisperx.load_model(model_size, device, compute_type=compute, language=language)
    wav = whisperx.load_audio(str(audio))
    result = model.transcribe(wav, batch_size=16)
    lang = result.get("language", language)
    align_model, metadata = whisperx.load_align_model(language_code=lang, device=device)
    result = whisperx.align(result["segments"], align_model, metadata, wav, device, return_char_alignments=False)

    hf_token = os.environ.get("HF_TOKEN")
    if hf_token:
        diarize = whisperx.DiarizationPipeline(use_auth_token=hf_token, device=device)
        result = whisperx.assign_word_speakers(diarize(wav), result)

    out: list[Segment] = []
    for i, s in enumerate(result["segments"]):
        words = [
            Word(start=float(w["start"]), end=float(w["end"]), text=w["word"].strip(), confidence=w.get("score"))
            for w in s.get("words", [])
            if "start" in w and "end" in w
        ]
        text = s.get("text", "").strip()
        if not text:
            continue
        out.append(
            Segment(
                id=i,
                source=source_key,
                start=float(s["start"]),
                end=float(s["end"]),
                text=text,
                speaker=s.get("speaker"),
                words=words,
            )
        )
    return out


def load_transcript_json(path: Path, source_key: str) -> list[Segment]:
    """Accepts our own analysis schema ({"segments": [...]}) or a bare list of segments."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    raw = data["segments"] if isinstance(data, dict) else data
    out: list[Segment] = []
    for i, s in enumerate(raw):
        words = [
            Word(
                start=float(w["start"]),
                end=float(w["end"]),
                text=str(w.get("text") or w.get("word") or "").strip(),
                confidence=w.get("confidence", w.get("probability", w.get("score"))),
            )
            for w in s.get("words", [])
        ]
        out.append(
            Segment(
                id=int(s.get("id", i)),
                source=s.get("source") or source_key,
                start=float(s["start"]),
                end=float(s["end"]),
                text=str(s["text"]).strip(),
                speaker=s.get("speaker"),
                words=words,
            )
        )
    return out


_TS_RE = re.compile(r"(\d{1,2}):(\d{2}):(\d{2})[.,](\d{1,3})")
_CUE_LINE_RE = re.compile(r"^\s*(?P<start>[\d:.,]+)\s*-->\s*(?P<end>[\d:.,]+)")


def _parse_ts(s: str) -> float:
    m = _TS_RE.search(s)
    if not m:
        raise ValueError(f"Bad subtitle timestamp: {s!r}")
    h, mi, se, ms = m.groups()
    return int(h) * 3600 + int(mi) * 60 + int(se) + int(ms.ljust(3, "0")) / 1000.0


def load_subtitle_file(path: Path, source_key: str) -> list[Segment]:
    """SRT or WebVTT -> cue-level segments (no word timings).

    Good enough for rough cuts: we cut at cue boundaries anyway. Speaker tags like
    "지효: 안녕" or VTT "<v 지효>안녕" are picked up when present.
    """
    text = Path(path).read_text(encoding="utf-8-sig")
    lines = text.splitlines()
    out: list[Segment] = []
    i = 0
    while i < len(lines):
        m = _CUE_LINE_RE.match(lines[i])
        if not m:
            i += 1
            continue
        start, end = _parse_ts(m.group("start")), _parse_ts(m.group("end"))
        i += 1
        body: list[str] = []
        while i < len(lines) and lines[i].strip():
            body.append(lines[i].strip())
            i += 1
        raw = " ".join(body)
        raw = re.sub(r"<[^>]+>", lambda t: "" if not t.group(0).startswith("<v ") else t.group(0), raw)
        speaker = None
        vm = re.match(r"<v\s+([^>]+)>\s*(.*)", raw)
        if vm:
            speaker, raw = vm.group(1).strip(), vm.group(2)
        else:
            sm = re.match(r"^([^:\s]{1,12}):\s+(.*)", raw)
            if sm:
                speaker, raw = sm.group(1), sm.group(2)
        raw = raw.strip()
        if not raw or end <= start:
            continue
        out.append(Segment(id=len(out), source=source_key, start=start, end=end, text=raw, speaker=speaker))
    return out


BACKENDS: dict[str, Transcriber] = {
    "faster-whisper": _faster_whisper,
    "whisperx": _whisperx,
}


def transcribe(backend: str, audio: Path, source_key: str, language: str | None, model_size: str) -> list[Segment]:
    if backend not in BACKENDS:
        raise ValueError(f"Unknown transcriber '{backend}'. Choose from {sorted(BACKENDS)} or 'json'.")
    return BACKENDS[backend](audio, source_key, language, model_size)  # type: ignore[call-arg]
