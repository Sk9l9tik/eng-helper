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
  std::optional<DictEntry> lookup(const std::string& raw_word) {
    std::string word = to_lower(strip_punct(raw_word));
    if (word.empty())
      return std::nullopt;

    if (auto entry = lookup_exact(word))
      return entry;
    for (const auto& variant : ocr_variants(word))
      if (auto entry = lookup_exact(variant))
        return entry;
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
    for (const auto& candidate : base_forms(word)) {
      auto it = index_.find(candidate);
      if (it == index_.end())
        continue;

      DictEntry entry;
      entry.headword = candidate;
      for (auto [offset, size] : it->second)
        parse_article(read_article(offset, size), entry);
      if (!entry.senses.empty())
        return entry;
    }
    return std::nullopt;
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
