#ifndef __ANKI_CONNECT__
#define __ANKI_CONNECT__

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "Dictionary.h"
#include "LLM.h" // escape_json, json_string_field

// Card for the Lapis note type (https://github.com/donkuri/lapis), added
// through AnkiConnect
struct AnkiNote {
  std::string word;
  std::string ipa;
  std::string definitions;  // HTML: English definitions from the dictionary
  std::string translations; // HTML: Russian translations from the dictionary
  std::string sentence;     // HTML: context with the word in <b>
  std::string in_context;   // HTML: translation of the word in this context and
                            // of the whole sentence
  std::string audio_path;   // local mp3, empty if none
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
                               const std::string& context,
                               const std::string& word_ru,
                               const std::string& sentence_ru) {
  using namespace anki_detail;
  AnkiNote note;
  note.word = entry ? entry->headword : word;
  note.ipa = entry ? entry->ipa : "";
  if (entry) {
    note.definitions = senses_list(entry->senses, false);
    note.translations = senses_list(entry->senses, true);
  }

  size_t at = word.empty() ? std::string::npos : context.find(word);
  note.sentence = at == std::string::npos
                      ? escape_html(context)
                      : escape_html(context.substr(0, at)) + "<b>" +
                            escape_html(word) + "</b>" +
                            escape_html(context.substr(at + word.size()));

  if (!word_ru.empty())
    note.in_context = "<b>" + escape_html(word_ru) + "</b>";
  if (!sentence_ru.empty()) {
    // the model marks the word's translation as "[...]"
    std::string ru;
    for (char c : escape_html(sentence_ru))
      ru += c == '[' ? "<b>" : c == ']' ? "</b>" : std::string(1, c);
    note.in_context += (note.in_context.empty() ? "" : "<br>") + ru;
  }
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

    std::string json =
        R"({"action":"addNote","version":6,"params":{"note":{"deckName":")" +
        escape_json(deck) + R"(","modelName":")" + escape_json(model) +
        R"(","tags":["lookupper"],"fields":{)" + field("Expression", n.word) +
        "," + field("ExpressionFurigana", n.word) + "," +
        field("ExpressionReading", n.ipa) + "," +
        field("MainDefinition", n.definitions) + "," +
        field("Glossary", n.translations) + "," +
        field("Sentence", n.sentence) + "," + field("MiscInfo", n.in_context) +
        "}";
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
