#ifndef __OCR_TEXT__
#define __OCR_TEXT__

// Recognized text shared by the OCR backends (Parser — tesseract, PaddleParser
// — PP-OCR) and the UI

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Rect {
  int x1, y1, x2, y2;

  Rect(int x1, int y1, int x2, int y2) : x1(x1), y1(y1), x2(x2), y2(y2) {}
  Rect() : x1(0), y1(0), x2(0), y2(0) {}
};

struct Block {
  int id;
  std::vector<std::string> words;
  std::string text;
  Rect box;

  Block(int id_, std::string text_, Rect box_)
      : id(id_), words{text_}, text{text_}, box(box_) {};
  Block() : id(0), text(), box({}), words{} {};
};

struct Word {
  std::string text;
  Rect box;
  int block_id;
  int para_id;
  Rect line; // box of the text line the word belongs to
  float confidence;
  std::string context; // sentence around the word, built from recognized words

  // Word(int id_, std::string text_, Rect box_) : id(id_), text(text_),
  // box(box_) {}; Word() : id(0), text(""), box({}) {};
};

inline void save_detected_words(const std::string& filename,
                                const std::vector<Block>& blocks,
                                const std::vector<Word>& words) {
  std::ofstream ofs(filename);
  if (!ofs)
    throw std::runtime_error("Cannot open output file");

  ofs << "Blocks:\n";
  for (const auto& b : blocks) {
    ofs << b.id << ": (" << b.box.x1 << "," << b.box.y1 << ") - (" << b.box.x2
        << "," << b.box.y2 << ")\n";
    ofs << [&b]() {
      std::string s;
      for (auto& it : b.text)
        s += it;
      return s;
    }() << "\n---\n";
    // ofs << b.text << "\n---\n";
  }

  ofs << "\nWords:\n";
  for (const auto& w : words) {
    ofs << "block_id=" << w.block_id << " conf=" << w.confidence << " text=\""
        << w.text << "\"\n";
    ofs << w.box.x1 << " " << w.box.y1 << "\n";
    ofs << w.box.x2 << " " << w.box.y2 << "\n\n";
  }
}

// Tesseract reads a standalone "I" in sans-serif fonts as "|": "Last time |
// checked". Between words of a sentence it is "I"; anywhere else ("File |
// Edit", table borders) it is a separator and is dropped.
inline void fix_misread_i(std::vector<Word>& words) {
  auto same_line = [](const Word& a, const Word& b) {
    return a.line.y1 == b.line.y1 && a.line.y2 == b.line.y2;
  };
  auto is_word = [](const std::string& s) {
    return !s.empty() && std::isalpha(static_cast<unsigned char>(s.back()));
  };
  std::vector<Word> out;
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i].text == "|") {
      bool prev_ok = !out.empty() && same_line(out.back(), words[i]) &&
                     is_word(out.back().text);
      bool next_ok =
          i + 1 < words.size() && same_line(words[i], words[i + 1]) &&
          std::islower(static_cast<unsigned char>(words[i + 1].text[0]));
      if (!prev_ok || !next_ok)
        continue;
      words[i].text = "I";
    }
    out.push_back(std::move(words[i]));
  }
  words = std::move(out);
}

inline bool ends_sentence(const std::string& word) {
  size_t e = word.find_last_not_of("\"')]»”’");
  if (e == std::string::npos)
    return false;
  if (word[e] == '!' || word[e] == '?')
    return true;
  if (word[e] != '.')
    return false;

  // abbreviations: "D.C.", "e.g.", "Mr."
  std::string w = word.substr(0, e);
  if (w.find('.') != std::string::npos)
    return false;
  static const char* titles[] = {"Mr",  "Mrs",  "Ms", "Dr",  "St",
                                 "Jr",  "Sr",   "vs", "etc", "Prof",
                                 "Gen", "Capt", "Lt", "Sgt"};
  for (const char* t : titles)
    if (w == t)
      return false;
  return true;
}

// "lack..." or "lack…": the sentence may go on ("and then... we left"), so the
// word after it decides (see starts_sentence)
inline bool ends_with_ellipsis(const std::string& word) {
  size_t e = word.find_last_not_of("\"')]»”’");
  if (e == std::string::npos)
    return false;
  const std::string w = word.substr(0, e + 1);
  return w.ends_with("...") || w.ends_with("\u2026");
}

// A Capitalized word. In ALL CAPS text (comics) and for "I" the case tells
// nothing.
inline bool starts_sentence(const std::string& t) {
  if (t.empty() || !std::isupper(static_cast<unsigned char>(t[0])) ||
      std::none_of(t.begin(), t.end(),
                   [](unsigned char c) { return std::islower(c); }))
    return false;
  return t != "I" && !(t.size() > 1 && t[0] == 'I' && t[1] == '\'');
}

// Does `next` continue the same text as `prev`? OCR engines sometimes put menu
// buttons, titles and other UI text into one paragraph with the real text, so
// layout is checked too. `h` is the line height of the clicked word. Line boxes
// are compared, not word boxes: "a" and "feigning" have very different heights
// in the same font.
inline bool continues(const Word& prev, const Word& next, int h) {
  // different font size: a title, a button, a caption
  int nh = next.line.y2 - next.line.y1;
  if (nh > h * 1.4 || nh * 1.4 < h)
    return false;

  bool same_line = next.line.y1 == prev.line.y1 && next.line.y2 == prev.line.y2;
  if (same_line)
    return prev.para_id == next.para_id &&
           next.box.x1 - prev.box.x2 < 2 * h; // not a separate column or widget
  if (next.line.y1 - prev.line.y2 >= h)
    return false; // usual line spacing
  if (prev.para_id == next.para_id)
    return true;

  // Subtitle lines have separate backgrounds, and the OCR puts each into its
  // own paragraph: the sentence goes on to the next paragraph if it is the next
  // line under this one, not a neighbouring column
  return next.line.x1 < prev.line.x2 && prev.line.x1 < next.line.x2;
}

// A line ended by Enter rather than by wrapping, and a new sentence after it:
// "Good evening, Alexander," / "Sorry for messaging so late." A wrapped line
// is full: the next word would not fit at its end. `right` is the right edge
// of the paragraph. Only a Capitalized next word counts (starts_sentence).
inline bool line_break_sentence(const Word& prev, const Word& next, int right) {
  if (next.line.y1 == prev.line.y1 && next.line.y2 == prev.line.y2)
    return false;
  if (!starts_sentence(next.text))
    return false;
  const int space = (prev.line.y2 - prev.line.y1) / 2;
  return prev.box.x2 + space + (next.box.x2 - next.box.x1) < right;
}

// The image is recognized in strips (see Parser::process), and words come
// strip by strip: a paragraph cut by a strip border goes on after the other
// paragraphs of its strip ("...we'll need to cancel", a column on the right,
// "tomorrow's classes..."), and its sentence would be cut. A paragraph that
// continues the text of another one (continues) is moved right after it.
inline void stitch_paragraphs(std::vector<Word>& words) {
  struct Run {
    size_t begin, end; // words of one paragraph, in a row
  };
  std::vector<Run> runs;
  for (size_t i = 0; i < words.size(); ++i)
    if (i == 0 || words[i].para_id != words[i - 1].para_id)
      runs.push_back({i, i + 1});
    else
      runs.back().end = i + 1;

  auto follows = [&](const Run& prev, const Run& next) {
    const Word &last = words[prev.end - 1], &head = words[next.begin];
    const int h = std::max(1, last.line.y2 - last.line.y1);
    return head.line.y1 > last.line.y1 && continues(last, head, h);
  };
  std::vector<bool> used(runs.size(), false);
  std::vector<Word> out;
  out.reserve(words.size());
  for (size_t r = 0; r < runs.size(); ++r) {
    for (size_t cur = r; cur < runs.size() && !used[cur];) {
      used[cur] = true;
      for (size_t k = runs[cur].begin; k < runs[cur].end; ++k)
        out.push_back(words[k]);
      // the next one in the order already continues it, or none does
      size_t next = cur + 1;
      if (next < runs.size() && !used[next] && follows(runs[cur], runs[next]))
        cur = next;
      else {
        next = runs.size();
        for (size_t c = cur + 2; c < runs.size() && next == runs.size(); ++c)
          if (!used[c] && follows(runs[cur], runs[c]))
            next = c;
        cur = next;
      }
    }
  }
  words = std::move(out);
}

// Context of a word is its sentence: neighbours in the same text up to . ! ?
// or a line break before a new sentence. Built from filtered words, so it
// spans line breaks and has no OCR noise from icons.
inline void build_contexts(std::vector<Word>& words) {
  // right edge of each paragraph, for line_break_sentence
  std::vector<int> right;
  for (const auto& w : words) {
    if (w.para_id < 0)
      continue;
    if (static_cast<size_t>(w.para_id) >= right.size())
      right.resize(w.para_id + 1, 0);
    right[w.para_id] = std::max(right[w.para_id], w.line.x2);
  }
  auto sentence_ends = [&](const Word& prev, const Word& next) {
    if (ends_sentence(prev.text))
      return true;
    // "they sorely lack... Donut joined the group"
    if (ends_with_ellipsis(prev.text) && starts_sentence(next.text))
      return true;
    if (prev.para_id < 0 || next.para_id < 0)
      return false;
    return line_break_sentence(
        prev, next, std::max(right[prev.para_id], right[next.para_id]));
  };

  const size_t max_side = 40;
  for (size_t i = 0; i < words.size(); ++i) {
    const int h = std::max(1, words[i].line.y2 - words[i].line.y1);
    size_t b = i;
    while (b > 0 && i - b < max_side && continues(words[b - 1], words[b], h) &&
           !sentence_ends(words[b - 1], words[b]))
      --b;
    size_t e = i;
    while (e + 1 < words.size() && e - i < max_side &&
           continues(words[e], words[e + 1], h) &&
           !sentence_ends(words[e], words[e + 1]))
      ++e;

    std::string context;
    for (size_t k = b; k <= e; ++k) {
      if (k > b)
        context += ' ';
      context += words[k].text;
    }
    words[i].context = std::move(context);
  }
}

// Words of another recognition pass added to `words`: new ones are appended
// (indices of the old ones stay), a duplicate replaces the old word only if it
// is much more confident. `order` is the reading order of `words`; a new word
// goes into the line of its neighbour. Contexts are rebuilt. Returns indices
// of the added and changed words.
inline std::vector<size_t> merge_words(std::vector<Word>& words,
                                       std::vector<size_t>& order,
                                       std::vector<Word> extra) {
  auto area = [](const Rect& r) {
    return double(r.x2 - r.x1) * double(r.y2 - r.y1);
  };
  // share of the smaller box covered by the other one
  auto overlap = [&](const Rect& a, const Rect& b) {
    const int w = std::min(a.x2, b.x2) - std::max(a.x1, b.x1),
              h = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    if (w <= 0 || h <= 0)
      return 0.0;
    return double(w) * h / std::max(1.0, std::min(area(a), area(b)));
  };
  auto same_line = [](const Rect& a, const Rect& b) {
    const int h = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    return h * 2 >= std::min(a.y2 - a.y1, b.y2 - b.y1);
  };

  int next_para = 0;
  for (const auto& w : words)
    next_para = std::max(next_para, w.para_id + 1);
  std::vector<std::pair<int, int>> para_map; // pass para id -> new id

  // another pass is a second guess: only confident real words are taken,
  // not "Fi" or "4" read from a border
  auto plausible = [](const Word& w) {
    const int letters = std::count_if(w.text.begin(), w.text.end(), [](char c) {
      return std::isalpha(static_cast<unsigned char>(c));
    });
    return w.confidence >= 70 &&
           (letters >= 2 || w.text == "I" || w.text == "A");
  };

  std::vector<size_t> changed;
  for (auto& e : extra) {
    if (!plausible(e))
      continue;
    size_t dup = words.size();
    for (size_t i = 0; i < words.size() && dup == words.size(); ++i)
      if (overlap(e.box, words[i].box) > 0.3)
        dup = i;
    if (dup < words.size()) {
      if (e.confidence > words[dup].confidence + 20) {
        words[dup].text = e.text;
        words[dup].confidence = e.confidence;
        words[dup].box = e.box;
        changed.push_back(dup);
      }
      continue;
    }

    // the nearest word on the same line, not in another column
    const int h = std::max(1, e.line.y2 - e.line.y1);
    size_t mate = words.size();
    int best = 3 * h;
    for (size_t i = 0; i < words.size(); ++i) {
      if (!same_line(e.line, words[i].line))
        continue;
      const int gap = std::max(words[i].box.x1 - e.box.x2,
                               e.box.x1 - words[i].box.x2);
      if (gap < best) {
        best = gap;
        mate = i;
      }
    }

    size_t at = order.size(); // position in the reading order
    if (mate < words.size()) {
      const Word& m = words[mate];
      e.para_id = m.para_id;
      e.line = Rect(std::min(m.line.x1, e.box.x1), m.line.y1,
                    std::max(m.line.x2, e.box.x2), m.line.y2);
      // after the last word of the line to the left of it, or before the line
      const size_t none = order.size() + 1;
      size_t first = none, after = none;
      for (size_t k = 0; k < order.size(); ++k) {
        const Word& o = words[order[k]];
        if (o.para_id != m.para_id || o.line.y1 != m.line.y1 ||
            o.line.y2 != m.line.y2)
          continue;
        if (first == none)
          first = k;
        if (o.box.x1 < e.box.x1)
          after = k + 1;
      }
      at = after != none ? after : first != none ? first : order.size();
    } else {
      // a line of its own: before the first line below it
      for (at = 0; at < order.size(); ++at)
        if (words[order[at]].line.y1 >= e.line.y2)
          break;
      auto it = std::find_if(para_map.begin(), para_map.end(),
                             [&](auto& p) { return p.first == e.para_id; });
      if (it == para_map.end())
        it = para_map.insert(para_map.end(), {e.para_id, next_para++});
      e.para_id = it->second;
    }

    words.push_back(std::move(e));
    order.insert(order.begin() + at, words.size() - 1);
    changed.push_back(words.size() - 1);
  }

  std::vector<Word> seq;
  for (size_t i : order)
    seq.push_back(words[i]);
  build_contexts(seq);
  for (size_t k = 0; k < order.size(); ++k)
    words[order[k]].context = std::move(seq[k].context);
  return changed;
}

#endif // __OCR_TEXT__
