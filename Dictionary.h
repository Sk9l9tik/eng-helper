#ifndef __STARDICT_DICTIONARY__
#define __STARDICT_DICTIONARY__

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>

// One meaning of a word: English definition and its Russian translations
struct Sense {
  std::string pos;                       // part of speech: noun, verb, ...
  std::string definition;                // English
  std::vector<std::string> translations; // Russian
};

struct DictEntry {
  std::string headword;
  std::string ipa;
  std::vector<Sense> senses;
};

// Reader for StarDict dictionaries (.ifo/.idx[.gz]/.dict) with
// sametypesequence=h (HTML articles), e.g. FreeDict eng-rus:
// https://freedict.org
class Dictionary {
public:
  // base_path without extension: ".../eng-rus/eng-rus"
  bool load(const std::string& base_path) {
    index_.clear();
    std::string idx;
    if (!read_gz(base_path + ".idx.gz", idx) &&
        !read_file(base_path + ".idx", idx))
      return false;

    dict_.open(base_path + ".dict", std::ios::binary);
    if (!dict_)
      return false;

    // idx record: word '\0' offset(be32) size(be32)
    size_t i = 0;
    while (i < idx.size()) {
      size_t end = idx.find('\0', i);
      if (end == std::string::npos || end + 9 > idx.size())
        break;
      auto be32 = [&](size_t p) {
        return (uint32_t(uint8_t(idx[p])) << 24) |
               (uint32_t(uint8_t(idx[p + 1])) << 16) |
               (uint32_t(uint8_t(idx[p + 2])) << 8) |
               uint32_t(uint8_t(idx[p + 3]));
      };
      index_[to_lower(idx.substr(i, end - i))].push_back(
          {be32(end + 1), be32(end + 5)});
      i = end + 9;
    }
    return !index_.empty();
  }

  bool loaded() const { return !index_.empty(); }

  // Looks up the word as is, then its base forms: "copiously" -> "copious",
  // "shoes" -> "shoe", then with typical OCR mistakes fixed: "seeklng" ->
  // "seeking"
  // Contractions fall back to their main word: "we'll" -> "we", "wouldn't" ->
  // "would"
  std::optional<DictEntry> lookup(const std::string& raw_word) {
    std::string word = normalize(raw_word);
    if (word.empty())
      return std::nullopt;

    for (const auto& w : {word, uncontracted(word)}) {
      if (w.empty())
        continue;
      if (auto entry = lookup_exact(w))
        return entry;
      for (const auto& variant : ocr_variants(w))
        if (auto entry = lookup_exact(variant))
          return entry;
    }
    return std::nullopt;
  }

  // OCR misreads fixed with the dictionary, word by word: "THIS DEMON WAS
  // INDEEP INTIMIPATED BY ME/" -> "THIS DEMON WAS INDEED INTIMIDATED BY ME!".
  // Known words, and unknown ones without a single close dictionary word
  // (names, slang), stay as they are.
  std::string correct_text(const std::string& text) const {
    std::string out, token;
    auto flush = [&] {
      if (token.empty())
        return;
      const std::string core = strip_punct(token);
      if (!core.empty()) {
        const size_t at = token.find(core);
        token.replace(at, core.size(), correct_word(core));
        // "!" read as "/" at the end of a word: "ME/"
        if (token.back() == '/' && at + core.size() == token.size() - 1)
          token.back() = '!';
      }
      out += token;
      token.clear();
    };
    for (char c : text) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        flush();
        out += c;
      } else {
        token += c;
      }
    }
    flush();
    return out;
  }

  // Lowercase, typographic apostrophes as "'", no punctuation or quotes
  // around: "‘Tomorrow’s’" -> "tomorrow's"
  static std::string normalize(const std::string& raw_word) {
    static const char* apostrophes[] = {"\u2019", "\u2018", "\u02BC", "\u00B4",
                                        "`"};
    std::string w = raw_word;
    for (const char* a : apostrophes)
      for (size_t at; (at = w.find(a)) != std::string::npos;)
        w.replace(at, std::strlen(a), "'");
    w = to_lower(strip_punct(w));
    // quotes around the word, "classes'" (plural possessive) -> "classes"
    size_t b = w.find_first_not_of('\''), e = w.find_last_not_of('\'');
    return b == std::string::npos ? "" : w.substr(b, e - b + 1);
  }

  // Phrasal verb starting with the word: "fell" + {"for", "her"} -> "fall
  // for". `next` are the words right after it. Returns the entry and how many
  // of `next` it takes
  std::optional<std::pair<DictEntry, size_t>>
  lookup_phrasal(const std::string& raw_word,
                 const std::vector<std::string>& next) {
    static const char* particles[] = {
        "up",   "down", "out",     "off",     "on",     "in",    "over",
        "away", "back", "along",   "through", "around", "about", "apart",
        "for",  "into", "forward", "after",   "across", "by"};
    std::string word = normalize(raw_word);
    if (word.empty() || next.empty() ||
        std::find(std::begin(particles), std::end(particles),
                  to_lower(next[0])) == std::end(particles))
      return std::nullopt;

    std::vector<std::string> bases = base_forms(word);
    if (auto it = irregular_verbs().find(word); it != irregular_verbs().end())
      bases.insert(bases.begin(), it->second);
    // "get along with" before "get along"
    for (size_t n = std::min<size_t>(next.size(), 2); n > 0; --n) {
      std::string tail;
      for (size_t i = 0; i < n; ++i)
        tail += " " + to_lower(next[i]);
      for (const auto& base : bases)
        if (auto entry = read_entry(base + tail))
          return std::pair{*entry, n};
    }
    return std::nullopt;
  }

  static std::string strip_punct(const std::string& s) {
    auto is_word_char = [](unsigned char c) {
      return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
    };
    size_t b = 0, e = s.size();
    while (b < e && !is_word_char(s[b]))
      ++b;
    while (e > b && !is_word_char(s[e - 1]))
      --e;
    return s.substr(b, e - b);
  }

private:
  // "we'll" -> "we", "wouldn't" -> "would", "won't" -> "will", "" if the word
  // is not a contraction
  static std::string uncontracted(const std::string& w) {
    static const std::pair<const char*, const char*> irregular[] = {
        {"won't", "will"}, {"can't", "can"}, {"shan't", "shall"},
        {"ain't", "be"}};
    for (auto [from, to] : irregular)
      if (w == from)
        return to;
    for (const char* suffix : {"n't", "'m", "'re", "'ve", "'ll", "'d", "'s"}) {
      const size_t n = std::strlen(suffix);
      if (w.size() > n && w.compare(w.size() - n, n, suffix) == 0)
        return w.substr(0, w.size() - n);
    }
    return "";
  }

public:
  // A word with a misread letter or two -> the dictionary word it was, in the
  // same case: "INDEEP" -> "INDEED". First the usual OCR confusions, then any
  // one letter, if that gives exactly one dictionary word.
  std::string correct_word(const std::string& word) const {
    // "MIP-BTAGE" -> "MID-STAGE": each part on its own
    if (const size_t dash = word.find('-');
        dash != std::string::npos && !known(normalize(word)))
      return correct_word(word.substr(0, dash)) + "-" +
             correct_word(word.substr(dash + 1));

    const std::string w = normalize(word);
    if (w.size() < 4 || known(w) ||
        std::any_of(w.begin(), w.end(), [](unsigned char c) {
          return !std::isalnum(c) && c != '\'' && c != '-';
        }))
      return word;

    static const std::pair<const char*, const char*> confusions[] = {
        {"p", "d"},  {"d", "p"},  {"rn", "m"}, {"m", "rn"}, {"cl", "d"},
        {"vv", "w"}, {"l", "i"},  {"i", "l"},  {"1", "l"},  {"1", "i"},
        {"0", "o"},  {"5", "s"},  {"8", "b"},  {"e", "c"},  {"c", "e"},
        {"u", "v"},  {"v", "u"},  {"h", "n"},  {"n", "h"},  {"t", "f"},
        {"f", "t"},  {"k", "x"},  {"g", "q"}};
    std::string fixed;
    for (auto [from, to] : confusions) {
      const size_t n = std::strlen(from);
      for (size_t at = w.find(from); at != std::string::npos && fixed.empty();
           at = w.find(from, at + 1)) {
        std::string v = w;
        v.replace(at, n, to);
        if (known(v))
          fixed = v;
      }
      if (!fixed.empty())
        break;
    }
    // any letter: too many short words are one letter apart
    if (fixed.empty() && w.size() >= 5) {
      int found = 0;
      for (size_t at = 0; at < w.size() && found < 2; ++at)
        for (char c = 'a'; c <= 'z' && found < 2; ++c) {
          if (c == w[at])
            continue;
          std::string v = w;
          v[at] = c;
          if (known(v) && v != fixed) {
            fixed = v;
            ++found;
          }
        }
      if (found != 1)
        fixed.clear();
    }
    if (fixed.empty())
      return word;

    // the case of the original: "INDEEP" -> "INDEED", "Indeep" -> "Indeed"
    const bool has_lower = std::any_of(word.begin(), word.end(), [](unsigned char c) {
      return std::islower(c);
    });
    if (!has_lower)
      for (char& c : fixed)
        c = std::toupper(static_cast<unsigned char>(c));
    else if (std::isupper(static_cast<unsigned char>(word[0])))
      fixed[0] = std::toupper(static_cast<unsigned char>(fixed[0]));
    return fixed;
  }

private:
  // Is there an entry for the word or its base form, without reading it
  bool known(const std::string& w) const {
    if (index_.count(w) || irregular_verbs().count(w))
      return true;
    if (const std::string u = uncontracted(w); !u.empty() && index_.count(u))
      return true;
    for (const auto& b : base_forms(w))
      if (index_.count(b))
        return true;
    return false;
  }

  // one-character substitutions of letters tesseract often confuses
  static std::vector<std::string> ocr_variants(const std::string& w) {
    static const std::pair<char, char> swaps[] = {
        {'l', 'i'}, {'i', 'l'}, {'1', 'l'}, {'1', 'i'}, {'0', 'o'}, {'5', 's'}};
    std::vector<std::string> out;
    for (size_t i = 0; i < w.size(); ++i)
      for (auto [from, to] : swaps)
        if (w[i] == from) {
          std::string v = w;
          v[i] = to;
          out.push_back(std::move(v));
        }
    return out;
  }

  std::optional<DictEntry> lookup_exact(const std::string& word) {
    if (auto entry = read_entry(word))
      return entry;
    // "would", "has": forms the dictionary has no entry for. Before the
    // suffixes, which make "has" -> "ha"
    if (auto it = irregular_verbs().find(word); it != irregular_verbs().end())
      if (auto entry = read_entry(it->second))
        return entry;
    for (const auto& candidate : base_forms(word))
      if (auto entry = read_entry(candidate))
        return entry;
    return std::nullopt;
  }

  std::optional<DictEntry> read_entry(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end())
      return std::nullopt;

    DictEntry entry;
    entry.headword = key;
    for (auto [offset, size] : it->second)
      parse_article(read_article(offset, size), entry);
    if (entry.senses.empty())
      return std::nullopt;
    return entry;
  }

  // forms of common verbs, base_forms() covers the regular ones
  static const std::unordered_map<std::string, std::string>& irregular_verbs() {
    static const std::unordered_map<std::string, std::string> forms = {
        {"fell", "fall"},     {"fallen", "fall"},   {"got", "get"},
        {"gotten", "get"},    {"gave", "give"},     {"given", "give"},
        {"went", "go"},       {"gone", "go"},       {"came", "come"},
        {"took", "take"},     {"taken", "take"},    {"made", "make"},
        {"put", "put"},       {"set", "set"},       {"ran", "run"},
        {"broke", "break"},   {"broken", "break"},  {"brought", "bring"},
        {"threw", "throw"},   {"thrown", "throw"},  {"held", "hold"},
        {"kept", "keep"},     {"let", "let"},       {"left", "leave"},
        {"stood", "stand"},   {"sat", "sit"},       {"woke", "wake"},
        {"woken", "wake"},    {"wore", "wear"},     {"worn", "wear"},
        {"cut", "cut"},       {"shut", "shut"},     {"blew", "blow"},
        {"blown", "blow"},    {"drew", "draw"},     {"drawn", "draw"},
        {"drove", "drive"},   {"driven", "drive"},  {"ate", "eat"},
        {"eaten", "eat"},     {"found", "find"},    {"fought", "fight"},
        {"grew", "grow"},     {"grown", "grow"},    {"hung", "hang"},
        {"laid", "lay"},      {"led", "lead"},      {"lit", "light"},
        {"paid", "pay"},      {"rode", "ride"},     {"ridden", "ride"},
        {"rose", "rise"},     {"risen", "rise"},    {"saw", "see"},
        {"seen", "see"},      {"sold", "sell"},     {"sent", "send"},
        {"shook", "shake"},   {"shaken", "shake"},  {"shot", "shoot"},
        {"spoke", "speak"},   {"spoken", "speak"},  {"stuck", "stick"},
        {"struck", "strike"}, {"swore", "swear"},   {"sworn", "swear"},
        {"told", "tell"},     {"thought", "think"}, {"tore", "tear"},
        {"torn", "tear"},     {"wound", "wind"},
        {"won", "win"},       {"wrote", "write"},   {"written", "write"},
        {"did", "do"},        {"done", "do"},       {"knew", "know"},
        {"known", "know"},    {"caught", "catch"},  {"bought", "buy"},
        {"built", "build"},   {"dug", "dig"},       {"fed", "feed"},
        {"felt", "feel"},     {"flew", "fly"},      {"flown", "fly"},
        {"forgot", "forget"}, {"heard", "hear"},    {"hid", "hide"},
        {"hidden", "hide"},   {"meant", "mean"},    {"met", "meet"},
        {"slept", "sleep"},   {"spent", "spend"},   {"swept", "sweep"},
        {"was", "be"},        {"were", "be"},
        {"been", "be"},       {"is", "be"},         {"are", "be"},
        {"am", "be"},         {"has", "have"},      {"had", "have"},
        {"does", "do"},       {"would", "will"},    {"could", "can"},
        {"might", "may"}};
    return forms;
  }

  static std::string to_lower(std::string s) {
    for (auto& c : s)
      c = std::tolower(static_cast<unsigned char>(c));
    return s;
  }

  static std::vector<std::string> base_forms(const std::string& w) {
    std::vector<std::string> out{w};
    auto ends = [&](const std::string& suf) {
      return w.size() > suf.size() + 1 &&
             w.compare(w.size() - suf.size(), suf.size(), suf) == 0;
    };
    auto add = [&](const std::string& suf, const std::string& rep) {
      if (!ends(suf))
        return;
      std::string base = w.substr(0, w.size() - suf.size());
      out.push_back(base + rep);
      // running -> run, stopped -> stop
      if (rep.empty() && base.size() > 2 &&
          base.back() == base[base.size() - 2])
        out.push_back(base.substr(0, base.size() - 1));
    };
    add("'s", "");
    add("ies", "y");
    add("ied", "y");
    add("ier", "y");
    add("iest", "y");
    add("ily", "y");
    add("ly", "");
    add("es", "");
    add("s", "");
    add("ed", "");
    add("ed", "e");
    add("d", "");
    add("ing", "");
    add("ing", "e");
    add("er", "");
    add("est", "");
    add("ness", "");
    return out;
  }

  std::string read_article(uint32_t offset, uint32_t size) {
    std::string buf(size, '\0');
    dict_.clear();
    dict_.seekg(offset);
    dict_.read(buf.data(), size);
    return buf;
  }

  static std::string decode_entities(std::string s) {
    static const std::pair<const char*, const char*> ents[] = {
        {"&lt;", "<"},  {"&gt;", ">"},   {"&quot;", "\""}, {"&apos;", "'"},
        {"&#39;", "'"}, {"&nbsp;", " "}, {"&amp;", "&"}};
    for (auto [from, to] : ents) {
      size_t pos = 0;
      while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, strlen(from), to);
        pos += strlen(to);
      }
    }
    return s;
  }

  static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\n\r"),
           e = s.find_last_not_of(" \t\n\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
  }

  // "[[помощь|по́мощи]]" -> "по́мощи", "[[слово]]" -> "слово"
  static std::string strip_wiki_links(std::string s) {
    size_t b;
    while ((b = s.find("[[")) != std::string::npos) {
      size_t e = s.find("]]", b);
      if (e == std::string::npos)
        break;
      std::string inner = s.substr(b + 2, e - b - 2);
      size_t bar = inner.rfind('|');
      s.replace(b, e - b + 2,
                bar == std::string::npos ? inner : inner.substr(bar + 1));
    }
    return s;
  }

  static std::string strip_stress(std::string s) {
    const std::string acute = "\xCC\x81"; // U+0301 combining acute accent
    size_t p;
    while ((p = s.find(acute)) != std::string::npos)
      s.erase(p, acute.size());
    return s;
  }

  static void add_translation(Sense& sense, const std::string& text) {
    for (const auto& t : sense.translations)
      if (strip_stress(t) == strip_stress(text))
        return;
    sense.translations.push_back(text);
  }

  // FreeDict articles look like:
  // <div>/<font color="gray">IPA</font>/<br><div><font
  // class="grammar">noun</font></div>
  //   Definition<ol><li><div>перевод</div></li></ol>
  //   <ol><li><ol><li>Definition 1</li><li>Definition
  //   2</li></ol><ol><li><div>перевод</div></li></ol></li>...
  // Text inside a nested <div> is a translation, other text is an English
  // definition. Definitions nested in one <li> share the translations that
  // follow them.
  static void parse_article(const std::string& html, DictEntry& entry) {
    std::vector<std::string> stack;
    std::vector<int>
        groups; // group of each sense added by this article, 0 = standalone
    const size_t first_sense = entry.senses.size();
    int outer_li = 0;
    std::string pos;
    bool in_gray = false, in_grammar = false;
    size_t i = 0;

    while (i < html.size()) {
      if (html[i] == '<') {
        size_t end = html.find('>', i);
        if (end == std::string::npos)
          break;
        std::string tag = html.substr(i + 1, end - i - 1);
        i = end + 1;

        bool closing = !tag.empty() && tag[0] == '/';
        std::string name = to_lower(tag.substr(
            closing ? 1 : 0,
            tag.find_first_of(" /", closing ? 1 : 0) - (closing ? 1 : 0)));
        if (name == "br")
          continue;

        if (!closing) {
          stack.push_back(name);
          if (name == "li" && std::count(stack.begin(), stack.end(), "li") == 1)
            ++outer_li;
          if (name == "font") {
            in_grammar = tag.find("grammar") != std::string::npos;
            in_gray = !in_grammar && tag.find("gray") != std::string::npos;
          }
        } else {
          auto it = std::find(stack.rbegin(), stack.rend(), name);
          if (it != stack.rend())
            stack.erase(std::next(it).base(), stack.end());
          if (name == "font")
            in_gray = in_grammar = false;
        }
        continue;
      }

      size_t end = html.find('<', i);
      std::string text =
          trim(strip_wiki_links(decode_entities(html.substr(i, end - i))));
      i = end == std::string::npos ? html.size() : end;
      if (text.empty() || text.find_first_not_of("/,;. ") == std::string::npos)
        continue;

      auto& senses = entry.senses;
      if (in_gray) {
        if (entry.ipa.empty())
          entry.ipa = "/" + text + "/";
      } else if (in_grammar) {
        pos = text;
      } else if (!stack.empty() && stack.back() == "div" &&
                 std::count(stack.begin(), stack.end(), "div") >= 2) {
        if (senses.size() == first_sense || senses.back().pos != pos) {
          senses.push_back({pos, "", {}});
          groups.push_back(0);
        }
        // definitions of one group without translations yet are merged into one
        // sense
        int g = groups.back();
        if (g != 0 && senses.back().translations.empty()) {
          while (groups.size() >= 2 && groups[groups.size() - 2] == g &&
                 senses[senses.size() - 2].translations.empty()) {
            senses[senses.size() - 2].definition +=
                " " + senses.back().definition;
            senses.pop_back();
            groups.pop_back();
          }
        }
        add_translation(senses.back(), text);
      } else {
        bool nested = std::count(stack.begin(), stack.end(), "li") >= 2;
        senses.push_back({pos, text, {}});
        groups.push_back(nested ? outer_li : 0);
      }
    }
  }

  static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
      return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
  }

  static bool read_gz(const std::string& path, std::string& out) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f)
      return false;
    char buf[1 << 16];
    int n;
    while ((n = gzread(f, buf, sizeof(buf))) > 0)
      out.append(buf, n);
    gzclose(f);
    return true;
  }

  std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>>
      index_;
  std::ifstream dict_;
};

#endif // __STARDICT_DICTIONARY__
