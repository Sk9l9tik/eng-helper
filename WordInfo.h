#ifndef __WORD_INFO__
#define __WORD_INFO__

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Text file of lines "key\tcolumn\t...", sorted by key bytewise (see
// tools/fetch_dictionaries.py), looked up by binary search right in the file:
// nothing is loaded at start, the Wiktionary one is a few hundred MB
class SortedTsv {
public:
  bool open(const std::string& path) {
    f_.open(path, std::ios::binary);
    if (!f_)
      return false;
    f_.seekg(0, std::ios::end);
    size_ = f_.tellg();
    return size_ > 0;
  }

  bool loaded() const { return size_ > 0; }

  // the lines of `key`, split into columns, in the order of the file
  std::vector<std::vector<std::string>> find(const std::string& key) {
    std::vector<std::vector<std::string>> out;
    if (!loaded() || key.empty())
      return out;
    // the first offset whose line has a key >= `key`
    std::streamoff lo = 0, hi = size_;
    while (lo < hi) {
      const std::streamoff mid = lo + (hi - lo) / 2;
      if (key_of(line_at(line_start(mid))) < key)
        lo = mid + 1;
      else
        hi = mid;
    }
    for (std::streamoff at = line_start(lo); at < size_;) {
      const std::string line = line_at(at);
      if (key_of(line) != key)
        break;
      out.push_back(split(line, '\t'));
      at += line.size() + 1;
    }
    return out;
  }

  static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t at; (at = s.find(sep, from)) != std::string::npos; from = at + 1)
      out.push_back(s.substr(from, at - from));
    out.push_back(s.substr(from));
    return out;
  }

private:
  static std::string key_of(const std::string& line) {
    return line.substr(0, line.find('\t'));
  }

  // start of the first line at or after `off`
  std::streamoff line_start(std::streamoff off) {
    if (off <= 0)
      return 0;
    f_.clear();
    f_.seekg(off - 1);
    std::string skipped;
    std::getline(f_, skipped);
    return f_ ? std::streamoff(f_.tellg()) : size_;
  }

  std::string line_at(std::streamoff start) {
    if (start >= size_)
      return "\xff"; // after any key
    f_.clear();
    f_.seekg(start);
    std::string line;
    std::getline(f_, line);
    return line;
  }

  std::ifstream f_;
  std::streamoff size_ = 0;
};

// A part of the word: "notary" + "a lawyer who ...", "-ize" + "..."
struct Component {
  std::string part, meaning;
};

// What is shown about the word besides its translation: the card of the popup
// and of Anki
struct WordInfo {
  std::string lemma;      // dictionary form: "notarize" for "notarized"
  std::string pos;        // "verb"
  std::string inflection; // "simple past and past participle", empty for the
                          // dictionary form itself
  std::string level;      // CEFR: "B2"
  bool level_estimated = false; // from the word's frequency, not a CEFR list
  std::vector<std::string> labels;   // usage: "formal", "slang"
  std::vector<std::string> forms;    // "notarizes", "notarizing", ...
  std::vector<std::string> synonyms; // "certify"
  std::vector<Component> components;
  std::string definition; // in simple words, the main thing of the card
  std::string example;    // from the same dictionary as the definition
  std::vector<std::string> glosses; // full Wiktionary definitions

  // the definition and style are asked from the model (see
  // OllamaClient::explain)
  bool needs_model() const { return definition.empty(); }
};

// Wiktionary, Simple English Wiktionary and CEFR word lists, filled by
// tools/fetch_dictionaries.py
class Lexicon {
public:
  // `dir` is the data directory: wiktionary.tsv, simple.tsv, cefr.tsv,
  // frequency.tsv
  bool load(const std::string& dir) {
    const bool wikt = wikt_.open(dir + "/wiktionary.tsv");
    simple_.open(dir + "/simple.tsv");
    cefr_.open(dir + "/cefr.tsv");
    frequency_.open(dir + "/frequency.tsv");
    return wikt;
  }

  bool loaded() const { return wikt_.loaded(); }

  // `word` as clicked; `prev` is the word before it in the sentence, to tell
  // "a book" from "to book"; `headword` is what the Russian dictionary found,
  // for forms Wiktionary has no entry for
  WordInfo lookup(const std::string& word, const std::string& prev,
                  const std::string& headword) {
    WordInfo info;
    std::string key = lower(word);
    auto rows = wikt_.find(key);
    if (rows.empty() && !headword.empty() && lower(headword) != key) {
      key = lower(headword);
      rows = wikt_.find(key);
    }
    info.lemma = rows.empty() ? (headword.empty() ? key : headword) : key;
    const std::vector<std::string> preferred = pos_after(lower(prev));

    if (!rows.empty()) {
      auto row = pick(rows, preferred);
      // "notarized": a form of "notarize", the rest is the lemma's
      if (!row[kLemma].empty()) {
        info.inflection = short_inflection(row[kInflection], lower(prev));
        info.lemma = row[kLemma];
        if (auto lemma_rows = only_lemmas(wikt_.find(lower(info.lemma)));
            !lemma_rows.empty())
          row = pick(lemma_rows, {row[kPos]});
      }
      info.pos = row[kPos];
      if (row[kLemma].empty()) {
        for (auto& l : SortedTsv::split(row[kLabels], ','))
          if (!l.empty())
            info.labels.push_back(l);
        info.forms = items(row[kForms]);
        // "called-": a dash for a form the verb has not
        std::erase(info.forms, "-");
        info.synonyms = items(row[kSynonyms]);
        info.glosses = items(row[kGlosses]);
        for (const auto& part : items(row[kComponents]))
          info.components.push_back({part, meaning_of(part)});
      }
    }

    // simple words: only of the same part of speech, another one would be
    // another word
    for (const auto& r : simple_.find(lower(info.lemma)))
      if (r.size() > 3 && (info.pos.empty() || r[1] == info.pos)) {
        if (info.pos.empty())
          info.pos = r[1];
        const auto defs = items(r[2]);
        info.definition = defs.empty() ? "" : defs.front();
        info.example = r[3];
        break;
      }

    info.level = level_of(lower(info.lemma), info.pos);
    if (info.level.empty() && info.lemma != key)
      info.level = level_of(key, info.pos);
    // not on the lists: by how frequent the word is, the more frequent of its
    // forms ("notarized" is more frequent than "notarize")
    if (info.level.empty() && (!rows.empty() || !info.definition.empty())) {
      info.level = level_by_rank(std::min(rank_of(lower(info.lemma)),
                                          rank_of(key)));
      info.level_estimated = true;
    }
    return info;
  }

  // Phrasal verb starting with the word, for those the Russian dictionary
  // lacks: "roll" + {"out", "a"} -> {"roll out", 1}, "rolled" -> "roll out"
  // too. `next` are the words right after it, `headword` as for lookup().
  // Returns the dictionary form and how many of `next` it takes
  std::optional<std::pair<std::string, size_t>>
  lookup_phrasal(const std::string& word, const std::vector<std::string>& next,
                 const std::string& headword) {
    if (next.empty())
      return std::nullopt;
    std::vector<std::string> bases{lower(word)};
    for (const auto& r : wikt_.find(bases.front()))
      if (r.size() >= kColumns && !r[kLemma].empty() && r[kPos] == "verb")
        bases.push_back(lower(r[kLemma]));
    if (!headword.empty())
      bases.push_back(lower(headword));
    // "get along with" before "get along"
    for (size_t n = std::min<size_t>(next.size(), 2); n > 0; --n) {
      std::string tail;
      for (size_t i = 0; i < n; ++i)
        tail += " " + lower(next[i]);
      for (const auto& base : bases)
        for (const auto& r : wikt_.find(base + tail))
          if (r.size() >= kColumns && r[kPos] == "verb")
            return std::pair{base + tail, n};
    }
    return std::nullopt;
  }

  // Model's answer (OllamaClient::explain) added where the dictionaries had
  // nothing: "DEFINITION: ...\nSTYLE: Formal"
  static void apply_model_answer(WordInfo& info, const std::string& answer) {
    for (const auto& raw : SortedTsv::split(answer, '\n')) {
      std::string line = trim(raw);
      line.erase(std::remove(line.begin(), line.end(), '*'), line.end());
      auto value = [&](const std::string& name) -> std::string {
        if (lower(line).rfind(lower(name) + ":", 0) != 0)
          return "";
        return trim(line.substr(name.size() + 1));
      };
      if (std::string def = value("DEFINITION"); !def.empty()) {
        // "It means the job has been checked..." -> "The job has been..."
        for (const char* prefix :
             {"it means that ", "it means ", "this means that ", "this means ",
              "here it means ", "it is "})
          if (lower(def).rfind(prefix, 0) == 0) {
            def = def.substr(std::string(prefix).size());
            break;
          }
        if (!def.empty() && info.definition.empty()) {
          def[0] = std::toupper(static_cast<unsigned char>(def[0]));
          info.definition = def;
        }
      } else if (std::string style = lower(value("STYLE")); !style.empty()) {
        static const char* known[] = {"formal",    "informal", "slang",
                                      "technical", "literary", "old-fashioned"};
        if (info.labels.empty() &&
            std::find(std::begin(known), std::end(known), style) !=
                std::end(known))
          info.labels.push_back(style);
      }
    }
  }

private:
  // columns of wiktionary.tsv
  enum {
    kKey,
    kPos,
    kLemma,
    kInflection,
    kLabels,
    kForms,
    kSynonyms,
    kComponents,
    kGlosses,
    kColumns
  };

  static std::string lower(std::string s) {
    for (auto& c : s)
      c = std::tolower(static_cast<unsigned char>(c));
    return s;
  }

  static std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n\"");
    const size_t e = s.find_last_not_of(" \t\r\n\"");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
  }

  static std::vector<std::string> items(const std::string& s) {
    std::vector<std::string> out;
    for (auto& i : SortedTsv::split(s, '\x1f'))
      if (!i.empty())
        out.push_back(i);
    return out;
  }

  // Wiktionary's name of the form, short for the chip: "simple past and past
  // participle" -> "past participle" in "a notarized job", "had notarized"
  static std::string short_inflection(const std::string& s,
                                      const std::string& prev) {
    if (s.find("simple past and past participle") != std::string::npos) {
      static const char* participle_after[] = {
          "have", "has", "had", "having", "be",  "been", "being", "is",
          "are",  "was", "were", "am",    "get", "got",  "gets",  "'s",
          "'ve",  "'d",  "i've", "we've", "they've", "you've"};
      const bool participle =
          !pos_after(prev).empty() && pos_after(prev).front() != "verb";
      return participle || std::find(std::begin(participle_after),
                                     std::end(participle_after),
                                     prev) != std::end(participle_after)
                 ? "past participle"
                 : "past tense";
    }
    static const std::pair<const char*, const char*> names[] = {
        {"present participle", "present participle"},
        {"past participle", "past participle"},
        {"third-person singular", "3rd person singular"},
        {"simple past", "past tense"},
        {"comparative", "comparative"},
        {"superlative", "superlative"},
        {"plural", "plural"}};
    for (auto [from, to] : names)
      if (s.find(from) != std::string::npos)
        return to;
    return s;
  }

  // parts of speech the word after `prev` most likely is: "a notarized job"
  static std::vector<std::string> pos_after(const std::string& prev) {
    static const char* determiners[] = {
        "a",    "an",   "the",   "my",    "your", "his",   "her",
        "its",  "our",  "their", "this",  "that", "these", "those",
        "some", "any",  "no",    "every", "each", "another"};
    static const char* intensifiers[] = {"very",   "so",      "too",
                                         "really", "pretty",  "quite",
                                         "rather", "extremely", "most",
                                         "more",   "less",    "fairly"};
    static const char* before_verbs[] = {
        "to",    "will",  "would", "can",  "could", "must",  "should",
        "may",   "might", "shall", "do",   "does",  "did",   "don't",
        "i",     "you",   "we",    "they", "he",    "she",   "let's",
        "doesn't", "didn't", "won't", "can't", "please"};
    auto in = [&](const auto& list) {
      return std::find(std::begin(list), std::end(list), prev) != std::end(list);
    };
    if (in(determiners))
      return {"adjective", "noun"};
    if (in(intensifiers))
      return {"adjective", "adverb"};
    if (in(before_verbs))
      return {"verb"};
    return {};
  }

  // well-formed rows only, the first of a preferred part of speech
  static std::vector<std::string>
  pick(const std::vector<std::vector<std::string>>& rows,
       const std::vector<std::string>& preferred) {
    for (const auto& pos : preferred)
      for (const auto& r : rows)
        if (r.size() >= kColumns && r[kPos] == pos)
          return r;
    for (const auto& r : rows)
      if (r.size() >= kColumns)
        return r;
    return std::vector<std::string>(kColumns);
  }

  static std::vector<std::vector<std::string>>
  only_lemmas(std::vector<std::vector<std::string>> rows) {
    std::erase_if(rows, [](const auto& r) {
      return r.size() < kColumns || !r[kLemma].empty();
    });
    return rows;
  }

  // Short meaning of a part of the word: the simple definition, else the
  // first Wiktionary one, up to the end of its first sentence
  std::string meaning_of(const std::string& part) {
    const std::string key = lower(part);
    std::string text;
    for (const auto& r : simple_.find(key))
      if (r.size() > 2 && !items(r[2]).empty()) {
        text = items(r[2]).front();
        break;
      }
    if (text.empty())
      for (const auto& r : only_lemmas(wikt_.find(key)))
        if (!items(r[kGlosses]).empty()) {
          text = items(r[kGlosses]).front();
          break;
        }
    for (const char* end : {". ", "; "})
      if (const size_t at = text.find(end); at != std::string::npos)
        text = text.substr(0, at);
    while (!text.empty() && text.back() == '.')
      text.pop_back();
    if (text.size() > 90)
      text = text.substr(0, text.rfind(' ', 87)) + "…";
    return text;
  }

  // place in the frequency list, a big number for a word not on it
  int rank_of(const std::string& word) {
    for (const auto& r : frequency_.find(word))
      if (r.size() > 1)
        return std::atoi(r[1].c_str());
    return 1 << 30;
  }

  // CEFR-J lists A1-B2 vocabulary in full, so a word not on it is B2 at
  // least; then by rank, as the usual estimate (5000 words for B2, 10000 for
  // C1)
  static std::string level_by_rank(int rank) {
    if (rank <= 5000)
      return "B2";
    if (rank <= 10000)
      return "C1";
    return "C2";
  }

  std::string level_of(const std::string& word, const std::string& pos) {
    const auto rows = cefr_.find(word);
    for (const auto& r : rows)
      if (r.size() > 2 && r[1] == pos)
        return r[2];
    // the lists mark the lowest level of any part of speech otherwise
    std::string best;
    for (const auto& r : rows)
      if (r.size() > 2 && (best.empty() || r[2] < best))
        best = r[2];
    return best;
  }

  SortedTsv wikt_, simple_, cefr_, frequency_;
};

#endif // __WORD_INFO__
