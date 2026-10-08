"""Transcription backends. Each returns list[Segment] with word timings when available.

Backends:
  faster-whisper  local, CPU or GPU, word timestamps built in   (pip install roughcut[whisper])
  whisperx        local, GPU recommended, wav2vec2 alignment     (pip install roughcut[whisperx])
  json            load a transcript produced earlier (our schema or faster-whisper style)
"""

from __future__ import annotations

import json
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

    device = os.environ.get("ROUGHCUT_WHISPER_DEVICE", "auto")
    compute = os.environ.get("ROUGHCUT_WHISPER_COMPUTE", "default")
    model = WhisperModel(model_size, device=device, compute_type=compute)
    raw_segments, _info = model.transcribe(
        str(audio),
        language=language,
        word_timestamps=True,
        vad_filter=True,
    )
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


BACKENDS: dict[str, Transcriber] = {
    "faster-whisper": _faster_whisper,
    "whisperx": _whisperx,
}


def transcribe(backend: str, audio: Path, source_key: str, language: str | None, model_size: str) -> list[Segment]:
    if backend not in BACKENDS:
        raise ValueError(f"Unknown transcriber '{backend}'. Choose from {sorted(BACKENDS)} or 'json'.")
    return BACKENDS[backend](audio, source_key, language, model_size)  # type: ignore[call-arg]
