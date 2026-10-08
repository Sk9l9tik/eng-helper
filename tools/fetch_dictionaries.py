#!/usr/bin/env python3
"""Downloads the data LookUpper needs into ~/.local/share/lookupper (or $XDG_DATA_HOME/lookupper):

  eng-rus/          FreeDict eng-rus StarDict dictionary (English definitions + Russian translations)
  audio/index.tsv   word -> pronunciation mp3 on Wikimedia Commons, taken from Wiktionary (kaikki.org dump)
  audio/*.mp3       pronunciations; the app downloads them on demand when a card is added,
                    --all-audio downloads all of them now (~100k files, several GB)
  wiktionary.tsv    part of speech, base form, usage labels, forms, synonyms, components (from the
                    etymology) and definitions, from the same Wiktionary dump
  simple.tsv        definitions in simple words, from the Simple English Wiktionary
  cefr.tsv          CEFR level of ~9k words: CEFR-J (A1-B2) and Octanove (C1-C2) vocabulary profiles
  frequency.tsv     rank of the 50k most frequent words in subtitles (FrequencyWords), for the
                    level of words the CEFR lists do not have

The .tsv files are sorted by their first column (the lowercase word), the app looks words up by
binary search in the file. List items are separated by \x1f.

Only the standard library is used.
"""

import argparse
import csv
import gzip
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path

FREEDICT_URL = "https://download.freedict.org/dictionaries/eng-rus/2025.11.23/freedict-eng-rus-2025.11.23.stardict.tar.xz"
KAIKKI_URL = "https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl"
SIMPLE_URL = "https://kaikki.org/dictionary/downloads/simple/simple-extract.jsonl.gz"
CEFR_URLS = [
    "https://raw.githubusercontent.com/openlanguageprofiles/olp-en-cefrj/master/cefrj-vocabulary-profile-1.5.csv",
    "https://raw.githubusercontent.com/openlanguageprofiles/olp-en-cefrj/master/octanove-vocabulary-profile-c1c2-1.0.csv",
]
USER_AGENT = "LookUpper/1.0 (https://github.com/Sk9l9tik/eng-helper)"  # Wikimedia rejects requests without one

# accents in order of preference
ACCENT_TAGS = [
    {"US", "General-American", "California", "New-York", "Canada"},
    {"UK", "Received-Pronunciation", "Southern-England", "England"},
]
FREQUENCY_URL = "https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_50k.txt"


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


SEP = "\x1f"

# kaikki part of speech -> the name shown in the app (FreeDict's names)
POS_NAMES = {
    "adj": "adjective", "adv": "adverb", "prep": "preposition", "conj": "conjunction",
    "pron": "pronoun", "det": "determiner", "intj": "interjection", "num": "numeral",
    "abbrev": "abbreviation", "prep_phrase": "phrase", "contraction": "contraction",
}
SKIP_POS = {"name", "character", "symbol", "hard-redirect", "punct", "romanization"}

# usage labels shown as the word's style
STYLE_TAGS = [
    "formal", "informal", "colloquial", "slang", "vulgar", "offensive", "derogatory", "humorous",
    "literary", "poetic", "archaic", "dated", "obsolete", "rare", "euphemistic", "childish",
    "Internet", "jargon", "technical",
]
# form-of senses that are not the usual inflection: "book" as the past of "bake" in a dialect
ODD_TAGS = {"dialectal", "obsolete", "archaic", "nonstandard", "misspelling", "rare", "pronunciation-spelling", "eye-dialect"}
# forms that are not words: inflection tables, alternative spellings
SKIP_FORM_TAGS = {"table-tags", "inflection-template", "class", "romanization", "alternative", "canonical", "archaic", "obsolete", "dialectal"}
COMPONENT_TEMPLATES = {"af", "affix", "suffix", "prefix", "compound", "confix", "surf"}


def clean(text: str) -> str:
    return " ".join(text.replace(SEP, " ").split())


def pos_name(pos: str) -> str:
    return POS_NAMES.get(pos, pos)


def components(entry: dict) -> list[str]:
    """notarize: From notary + -ize -> ["notary", "-ize"]"""
    for t in entry.get("etymology_templates", []):
        name, args = t.get("name"), t.get("args", {})
        if args.get("1") != "en":
            continue
        nums = [args[k] for k in sorted((k for k in args if k.isdigit()), key=int) if k != "1"]
        if name == "ety":  # {{ety|en|:af|notary|-ize}}
            if not nums or nums[0] not in (":af", ":affix", ":compound", ":suffix", ":prefix"):
                continue
            kind, nums = nums[0][1:], nums[1:]
        elif name in COMPONENT_TEMPLATES:
            kind = name
        else:
            continue
        parts = [p.split("<")[0].strip() for p in nums]
        if kind == "suffix" and len(parts) >= 2:
            parts[1:] = ["-" + p.lstrip("-") for p in parts[1:]]
        elif kind == "prefix" and len(parts) >= 2:
            parts[:-1] = [p.rstrip("-") + "-" for p in parts[:-1]]
        # other languages ("la:nota") and empty parts: not a word split
        if len(parts) < 2 or any(not p or ":" in p for p in parts):
            continue
        return parts
    return []


def wiktionary_lines(entry: dict) -> list[str]:
    """One line per entry: word pos lemma inflection labels forms synonyms components glosses"""
    word, pos = entry.get("word", ""), entry.get("pos", "")
    if not word or pos in SKIP_POS:
        return []
    key = word.lower()
    senses = entry.get("senses", [])

    out = []
    # "notarized": simple past and past participle of notarize
    for s in senses:
        tags = set(s.get("tags", []))
        if "form-of" not in tags or tags & ODD_TAGS or not s.get("form_of"):
            continue
        lemma = s["form_of"][0].get("word", "")
        gloss = (s.get("glosses") or [""])[-1]
        inflection = gloss.rsplit(" of ", 1)[0] if " of " in gloss else ""
        if lemma and lemma.lower() != key:
            out.append("\t".join([key, pos_name(pos), clean(lemma), clean(inflection), "", "", "", "", ""]))

    real = [s for s in senses if "form-of" not in s.get("tags", []) and s.get("glosses")]
    if not real:
        return out
    first_tags = real[0].get("tags", [])
    labels = [t for t in STYLE_TAGS if t in first_tags]

    forms = []
    for f in entry.get("forms", []):
        form = f.get("form", "")
        if form and form.lower() != key and form not in forms and not set(f.get("tags", [])) & SKIP_FORM_TAGS:
            forms.append(form)

    synonyms = []
    # entry-wide ones of another sense ("sense": "a person directed by another") are not the word's
    entry_synonyms = [x for x in entry.get("synonyms", []) if "sense" not in x]
    for syn in entry_synonyms + [x for s in real[:1] for x in s.get("synonyms", [])]:
        w = syn.get("word", "")
        # words from the thesaurus pages are a long list of rare ones
        if w and w.lower() != key and w not in synonyms and "source" not in syn and not set(syn.get("tags", [])) & ODD_TAGS:
            synonyms.append(w)

    glosses = [clean(s["glosses"][-1])[:250] for s in real[:3]]
    out.append("\t".join([key, pos_name(pos), "", "", ",".join(labels), SEP.join(clean(f) for f in forms[:4]),
                          SEP.join(clean(w) for w in synonyms[:4]), SEP.join(clean(p) for p in components(entry)),
                          SEP.join(glosses)]))
    return out


def write_sorted(path: Path, lines: list[str]) -> None:
    # by the word, stable: the order of the dump (the main meaning first) stays
    lines.sort(key=lambda line: line.split("\t", 1)[0].encode())
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")
    tmp.rename(path)


def sort_file(unsorted: Path, path: Path) -> None:
    """Like write_sorted for a file too big to sort in memory"""
    tmp = path.with_suffix(".tmp")
    subprocess.run(["sort", "-s", "-t", "\t", "-k1,1", "-o", str(tmp), str(unsorted)],
                   env={**os.environ, "LC_ALL": "C"}, check=True)
    unsorted.unlink()
    tmp.rename(path)


def process_kaikki(out: Path) -> None:
    """One pass over the Wiktionary dump: audio/index.tsv and wiktionary.tsv"""
    audio_dir = out / "audio"
    index, wikt = audio_dir / "index.tsv", out / "wiktionary.tsv"
    need_audio, need_wikt = not index.exists(), not wikt.exists()
    if not need_audio:
        print("audio index: already present")
    if not need_wikt:
        print("wiktionary: already present")
    if not need_audio and not need_wikt:
        return
    audio_dir.mkdir(parents=True, exist_ok=True)

    print("wiktionary: streaming", KAIKKI_URL, "(~3.3 GB, only the index is kept)")
    best: dict[str, tuple[int, str]] = {}
    unsorted = wikt.with_suffix(".unsorted")
    entries = 0
    read = 0
    with open_url(KAIKKI_URL) as r, open(unsorted, "w", encoding="utf-8") as lines:
        for line in r:
            read += len(line)
            if read % (100 << 20) < len(line):
                print(f"\r  {read >> 20} MB, {entries} entries", end="", flush=True)
            # most entries have no audio, skip parsing them
            if not need_wikt and b'"mp3_url"' not in line:
                continue
            entry = json.loads(line)
            if entry.get("lang_code") != "en":
                continue
            if need_wikt:
                for w in wiktionary_lines(entry):
                    lines.write(w + "\n")
                    entries += 1
            if not need_audio:
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

    if need_audio:
        tmp = index.with_suffix(".tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            for word in sorted(best):
                f.write(f"{word}\t{best[word][1]}\n")
        tmp.rename(index)
        print(f"audio index: {len(best)} words")
    if need_wikt:
        sort_file(unsorted, wikt)
        print(f"wiktionary: {entries} entries")
    else:
        unsorted.unlink()


def fetch_simple(out: Path) -> None:
    """word pos definitions example"""
    path = out / "simple.tsv"
    if path.exists():
        print("simple: already present")
        return
    print("simple: downloading", SIMPLE_URL)
    with open_url(SIMPLE_URL) as r:
        data = gzip.decompress(r.read()).decode("utf-8")
    lines = []
    for line in data.splitlines():
        entry = json.loads(line)
        word, pos = entry.get("word", ""), entry.get("pos", "")
        if not word or pos in SKIP_POS:
            continue
        senses = [s for s in entry.get("senses", []) if s.get("glosses") and "form-of" not in s.get("tags", [])]
        if not senses:
            continue
        example = next((x["text"] for s in senses[:1] for x in s.get("examples", []) if x.get("text")), "")
        lines.append("\t".join([word.lower(), pos_name(pos), SEP.join(clean(s["glosses"][-1]) for s in senses[:3]), clean(example)]))
    write_sorted(path, lines)
    print(f"simple: {len(lines)} entries")


def fetch_cefr(out: Path) -> None:
    """word pos level"""
    path = out / "cefr.tsv"
    if path.exists():
        print("cefr: already present")
        return
    lines = []
    for url in CEFR_URLS:
        print("cefr: downloading", url)
        with open_url(url) as r:
            rows = csv.DictReader(io.StringIO(r.read().decode("utf-8")))
            for row in rows:
                level = row.get("CEFR", "").strip()
                # "a.m./A.M./am/AM"
                for word in {w.strip().lower() for w in row.get("headword", "").split("/")}:
                    if word and level:
                        lines.append("\t".join([word, row.get("pos", "").strip(), level]))
    write_sorted(path, lines)
    print(f"cefr: {len(lines)} words")


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


def fetch_frequency(out: Path) -> None:
    """word rank"""
    path = out / "frequency.tsv"
    if path.exists():
        print("frequency: already present")
        return
    print("frequency: downloading", FREQUENCY_URL)
    with open_url(FREQUENCY_URL) as r:
        words = [line.split(" ")[0] for line in r.read().decode("utf-8").splitlines() if line]
    write_sorted(path, [f"{w}\t{rank}" for rank, w in enumerate(words, 1)])
    print(f"frequency: {len(words)} words")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--all-audio", action="store_true", help="download every pronunciation now instead of on demand")
    args = parser.parse_args()

    out = data_dir()
    out.mkdir(parents=True, exist_ok=True)
    fetch_freedict(out)
    fetch_cefr(out)
    fetch_frequency(out)
    fetch_simple(out)
    process_kaikki(out)
    if args.all_audio:
        fetch_all_audio(out / "audio")


if __name__ == "__main__":
    main()
