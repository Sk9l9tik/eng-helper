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

// Context of a word is its sentence: neighbours in the same text up to . ! ?
// Built from filtered words, so it spans line breaks and has no OCR noise from
// icons.
inline void build_contexts(std::vector<Word>& words) {
  const size_t max_side = 40;
  for (size_t i = 0; i < words.size(); ++i) {
    const int h = std::max(1, words[i].line.y2 - words[i].line.y1);
    size_t b = i;
    while (b > 0 && i - b < max_side && continues(words[b - 1], words[b], h) &&
           !ends_sentence(words[b - 1].text))
      --b;
    size_t e = i;
    while (e + 1 < words.size() && e - i < max_side &&
           continues(words[e], words[e + 1], h) &&
           !ends_sentence(words[e].text))
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

#endif // __OCR_TEXT__
