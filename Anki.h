#ifndef __ANKI_CONNECT__
#define __ANKI_CONNECT__

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "Dictionary.h"
#include "WordInfo.h"
#include "LLM.h" // escape_json, json_string_field

// Card for the Lapis note type (https://github.com/donkuri/lapis), added
// through AnkiConnect. Lapis shows Expression with its reading, the picture,
// the sentence with the word in <b> (in color), then the definitions:
// MainDefinition first, Glossary on the next page (arrows)
struct AnkiNote {
  std::string word;           // dictionary form
  std::string ipa;
  std::string definitions;    // HTML: level, part of speech, style; the
                              // definition in simple English; synonyms, parts
  std::string translations;   // HTML: translation in context, Russian
                              // translations, full English definitions
  std::string sentence;       // HTML: context with the word in <b>
  std::string in_context;     // HTML: translation of the word in this context
                              // and of the whole sentence; forms
  std::string frequency;      // "CEFR C1", shown in the header
  std::vector<std::string> tags;
  std::string audio_path;     // local mp3, empty if none
  std::string picture_base64; // jpeg, empty if none
};

namespace anki_detail {

inline std::string escape_html(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    // Lapis pastes fields into JS template literals: `{{MainDefinition}}`
    case '`':
      out += "&#96;";
      break;
    case '$':
      out += "&#36;";
      break;
    default:
      out += c;
    }
  }
  return out;
}

inline std::string senses_list(const std::vector<Sense>& senses, bool russian) {
  std::string out;
  for (const auto& s : senses) {
    std::string text;
    if (russian) {
      for (const auto& t : s.translations)
        text += (text.empty() ? "" : ", ") + escape_html(t);
    } else {
      text = escape_html(s.definition);
    }
    if (text.empty())
      continue;
    out += "<li>" +
           (s.pos.empty() ? "" : "<i>" + escape_html(s.pos) + "</i> ") + text +
           "</li>";
  }
  return out.empty() ? "" : "<ol>" + out + "</ol>";
}

// "error": null  or  "error": "text"
inline std::string response_error(const std::string& json) {
  size_t i = json.find("\"error\":");
  if (i == std::string::npos)
    return "unexpected AnkiConnect response";
  i = json.find_first_not_of(' ', i + 8);
  if (i == std::string::npos || json[i] != '"')
    return "";
  return json_string_field("\"error\":" + json.substr(i), "error");
}

inline size_t append(void* data, size_t size, size_t n, void* out) {
  static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
  return size * n;
}

} // namespace anki_detail

inline AnkiNote make_anki_note(const std::string& word,
                               const std::optional<DictEntry>& entry,
                               const WordInfo& info,
                               const std::string& context,
                               const std::string& word_ru,
                               const std::string& sentence_ru) {
  using namespace anki_detail;
  AnkiNote note;
  note.word = !info.lemma.empty() ? info.lemma : entry ? entry->headword : word;
  note.ipa = entry ? entry->ipa : "";

  // Lapis' own tag boxes: "C1" "verb" "past participle" "formal"
  std::string tags;
  auto tag = [&](const std::string& t) {
    if (!t.empty())
      tags += "<span class=\"tags\">" + escape_html(t) + "</span> ";
  };
  tag(info.level);
  tag(info.pos);
  tag(info.inflection);
  for (const auto& l : info.labels)
    tag(l);

  std::string main;
  if (!tags.empty())
    main += "<div style=\"margin-bottom:0.4em\">" + tags + "</div>";
  // the simple definition, else the dictionary ones
  if (!info.definition.empty())
    main += "<div style=\"font-size:1.15em\">" +
            escape_html(info.definition) + "</div>";
  else if (!info.glosses.empty())
    main += "<div>" + escape_html(info.glosses.front()) + "</div>";
  else if (entry)
    main += senses_list(entry->senses, false);
  if (!info.example.empty())
    main += "<div style=\"opacity:0.7; font-style:italic\">" +
            escape_html(info.example) + "</div>";
  if (!info.synonyms.empty()) {
    std::string list;
    for (const auto& s : info.synonyms)
      list += (list.empty() ? "" : ", ") + escape_html(s);
    main += "<div style=\"margin-top:0.4em; opacity:0.8\">Synonyms: "
            "<span style=\"font-style:italic\">" +
            list + "</span></div>";
  }
  if (!info.components.empty()) {
    main += "<div style=\"margin-top:0.4em\">";
    for (const auto& c : info.components)
      main += "<div><span style=\"font-weight:600\">" + escape_html(c.part) +
              "</span>" +
              (c.meaning.empty() ? "" : " — " + escape_html(c.meaning)) +
              "</div>";
    main += "</div>";
  }
  note.definitions = main;

  // translation in context first, then the dictionaries
  std::string ru_sentence;
  if (!sentence_ru.empty())
    // the model marks the word's translation as "[...]"
    for (char c : escape_html(sentence_ru))
      ru_sentence += c == '[' ? "<b>" : c == ']' ? "</b>" : std::string(1, c);
  std::string gloss;
  if (!word_ru.empty() || !ru_sentence.empty())
    gloss += "<div>" +
             (word_ru.empty() ? "" : "<b>" + escape_html(word_ru) + "</b>") +
             (!word_ru.empty() && !ru_sentence.empty() ? "<br>" : "") +
             ru_sentence + "</div>";
  if (entry)
    gloss += senses_list(entry->senses, true);
  if (!info.glosses.empty()) {
    gloss += "<ol>";
    for (const auto& g : info.glosses)
      gloss += "<li>" + escape_html(g) + "</li>";
    gloss += "</ol>";
  } else if (entry && !info.definition.empty()) {
    gloss += senses_list(entry->senses, false);
  }
  note.translations = gloss;

  size_t at = word.empty() ? std::string::npos : context.find(word);
  note.sentence = at == std::string::npos
                      ? escape_html(context)
                      : escape_html(context.substr(0, at)) + "<b>" +
                            escape_html(word) + "</b>" +
                            escape_html(context.substr(at + word.size()));

  if (!word_ru.empty())
    note.in_context = "<b>" + escape_html(word_ru) + "</b>";
  if (!ru_sentence.empty())
    note.in_context += (note.in_context.empty() ? "" : "<br>") + ru_sentence;
  if (!info.forms.empty()) {
    std::string forms;
    for (const auto& f : info.forms)
      forms += (forms.empty() ? "" : ", ") + escape_html(f);
    note.in_context +=
        (note.in_context.empty() ? "" : "<br>") + std::string("Forms: ") + forms;
  }

  if (!info.level.empty())
    note.frequency = "CEFR " + info.level +
                     (info.level_estimated ? " (by frequency)" : "");
  // Anki tags have no spaces: "past participle" is not one
  note.tags = {"lookupper"};
  for (const auto& t : {info.level, info.pos})
    if (!t.empty() && t.find(' ') == std::string::npos)
      note.tags.push_back(t);
  for (const auto& l : info.labels)
    if (l.find(' ') == std::string::npos)
      note.tags.push_back(l);
  return note;
}

class AnkiClient {
public:
  std::string deck = "Mining";
  std::string model = "Lapis";

  // Throws with AnkiConnect's message, e.g. "cannot create note because it is a
  // duplicate"
  void add(const AnkiNote& n) const {
    auto field = [](const char* name, const std::string& value) {
      return "\"" + std::string(name) + "\":\"" + escape_json(value) + "\"";
    };
    const std::string file = file_stem(n.word);
    std::string tags;
    for (const auto& t : n.tags)
      tags += (tags.empty() ? "\"" : ",\"") + escape_json(t) + "\"";

    std::string json =
        R"({"action":"addNote","version":6,"params":{"note":{"deckName":")" +
        escape_json(deck) + R"(","modelName":")" + escape_json(model) +
        R"(","tags":[)" + tags + R"(],"fields":{)" +
        field("Expression", n.word) + "," +
        field("ExpressionFurigana", n.word) + "," +
        field("ExpressionReading", n.ipa) + "," +
        field("MainDefinition", n.definitions) + "," +
        field("Glossary", n.translations) + "," +
        field("Sentence", n.sentence) + "," +
        field("Frequency", n.frequency) + "," +
        field("MiscInfo", n.in_context) + "}";
    if (!n.audio_path.empty())
      json += R"(,"audio":[{"path":")" + escape_json(n.audio_path) +
              R"(","filename":"lookupper_)" + file +
              R"(.mp3","fields":["ExpressionAudio"]}])";
    if (!n.picture_base64.empty())
      json += R"(,"picture":[{"data":")" + n.picture_base64 +
              R"(","filename":"lookupper_)" + file + "_" +
              std::to_string(time(nullptr)) + R"(.jpg","fields":["Picture"]}])";
    json += "}}}";

    const std::string error = anki_detail::response_error(post(json));
    if (!error.empty())
      throw std::runtime_error(error);
  }

private:
  static std::string file_stem(std::string word) {
    for (auto& c : word)
      if (!std::isalnum(static_cast<unsigned char>(c)))
        c = '_';
    return word;
  }

  std::string post(const std::string& json) const {
    std::unique_ptr<CURL, void (*)(CURL*)> curl(curl_easy_init(),
                                                curl_easy_cleanup);
    if (!curl)
      throw std::runtime_error("curl init failed");
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, anki_detail::append);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    CURLcode res = curl_easy_perform(curl.get());
    if (res == CURLE_COULDNT_CONNECT)
      throw std::runtime_error("Anki is not running (AnkiConnect, port 8765)");
    if (res != CURLE_OK)
      throw std::runtime_error(std::string("AnkiConnect: ") +
                               curl_easy_strerror(res));
    return response;
  }

  std::string url_ = "http://127.0.0.1:8765";
};

#endif // __ANKI_CONNECT__
