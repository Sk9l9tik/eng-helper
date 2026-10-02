#!/usr/bin/env python3
"""Downloads the data LookUpper needs into ~/.local/share/lookupper (or $XDG_DATA_HOME/lookupper):

  eng-rus/          FreeDict eng-rus StarDict dictionary (English definitions + Russian translations)
  audio/index.tsv   word -> pronunciation mp3 on Wikimedia Commons, taken from Wiktionary (kaikki.org dump)
  audio/*.mp3       pronunciations; the app downloads them on demand when a card is added,
                    --all-audio downloads all of them now (~100k files, several GB)

Only the standard library is used.
"""

import argparse
import io
import json
import os
import re
import sys
import tarfile
import time
import urllib.request
from pathlib import Path

FREEDICT_URL = "https://download.freedict.org/dictionaries/eng-rus/2025.11.23/freedict-eng-rus-2025.11.23.stardict.tar.xz"
KAIKKI_URL = "https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl"
USER_AGENT = "LookUpper/1.0 (https://github.com/Sk9l9tik/eng-helper)"  # Wikimedia rejects requests without one

# accents in order of preference
ACCENT_TAGS = [
    {"US", "General-American", "California", "New-York", "Canada"},
    {"UK", "Received-Pronunciation", "Southern-England", "England"},
]


def data_dir() -> Path:
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return Path(base) / "lookupper"


def open_url(url: str):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": USER_AGENT}), timeout=60)


def fetch_freedict(out: Path) -> None:
    if (out / "eng-rus" / "eng-rus.ifo").exists():
        print("eng-rus: already present")
        return
    print("eng-rus: downloading", FREEDICT_URL)
    with open_url(FREEDICT_URL) as r:
        data = r.read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as tar:
        tar.extractall(out, filter="data")
    print("eng-rus: done")


def accent_rank(sound: dict) -> int:
    tags = set(sound.get("tags", []))
    for rank, accent in enumerate(ACCENT_TAGS):
        if tags & accent:
            return rank
    return len(ACCENT_TAGS)


def build_audio_index(audio_dir: Path) -> None:
    index = audio_dir / "index.tsv"
    if index.exists():
        print("audio index: already present")
        return
    audio_dir.mkdir(parents=True, exist_ok=True)

    print("audio index: streaming", KAIKKI_URL, "(~3.3 GB, only the index is kept)")
    best: dict[str, tuple[int, str]] = {}
    read = 0
    with open_url(KAIKKI_URL) as r:
        for line in r:
            read += len(line)
            if read % (100 << 20) < len(line):
                print(f"\r  {read >> 20} MB, {len(best)} words", end="", flush=True)
            if b'"mp3_url"' not in line:  # most entries have no audio, skip parsing them
                continue
            entry = json.loads(line)
            if entry.get("lang_code") != "en":
                continue
            word = entry["word"].lower()
            for sound in entry.get("sounds", []):
                url = sound.get("mp3_url")
                if not url:
                    continue
                rank = accent_rank(sound)
                if word not in best or rank < best[word][0]:
                    best[word] = (rank, url)
    print()

    tmp = index.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        for word in sorted(best):
            f.write(f"{word}\t{best[word][1]}\n")
    tmp.rename(index)
    print(f"audio index: {len(best)} words")


# same file name as Audio.h uses
def audio_file(audio_dir: Path, word: str) -> Path:
    return audio_dir / (re.sub(r"[^a-z0-9'-]", "_", word) + ".mp3")


def fetch_all_audio(audio_dir: Path) -> None:
    lines = (audio_dir / "index.tsv").read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines, 1):
        word, url = line.split("\t")
        path = audio_file(audio_dir, word)
        if path.exists():
            continue
        try:
            with open_url(url) as r:
                path.write_bytes(r.read())
        except Exception as e:
            print(f"\n  {word}: {e}", file=sys.stderr)
        if i % 100 == 0:
            print(f"\r  {i}/{len(lines)}", end="", flush=True)
        time.sleep(0.05)  # stay under Wikimedia's rate limit
    print()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--all-audio", action="store_true", help="download every pronunciation now instead of on demand")
    args = parser.parse_args()

    out = data_dir()
    out.mkdir(parents=True, exist_ok=True)
    fetch_freedict(out)
    build_audio_index(out / "audio")
    if args.all_audio:
        fetch_all_audio(out / "audio")


if __name__ == "__main__":
    main()
