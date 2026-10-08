#ifndef __OLLAMA_CLIENT__
#define __OLLAMA_CLIENT__

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <curl/curl.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using DataCallback = std::function<bool(const char*, size_t)>;

// Exceptions must not pass through libcurl (C code), so errors are reported by
// aborting the transfer
static size_t WriteCallback(void* contents, size_t size, size_t nmemb,
                            void* userp) {
  auto* on_data = static_cast<DataCallback*>(userp);
  if (*on_data && !(*on_data)(static_cast<const char*>(contents), size * nmemb))
    return 0;
  return size * nmemb;
}

static std::string escape_json(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '\"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20)
        continue; // other control chars are invalid in JSON
      out += c;
      break;
    }
  }
  return out;
}

static void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80)
    out += char(cp);
  else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Value of a string field in a flat JSON object: {"response":"текст\n",...} ->
// "текст\n"
static std::string json_string_field(const std::string& json,
                                     const std::string& key) {
  std::string pattern = "\"" + key + "\":\"";
  size_t i = json.find(pattern);
  if (i == std::string::npos)
    return "";
  i += pattern.size();

  std::string out;
  while (i < json.size() && json[i] != '"') {
    char c = json[i++];
    if (c != '\\' || i >= json.size()) {
      out += c;
      continue;
    }
    char e = json[i++];
    switch (e) {
    case 'n':
      out += '\n';
      break;
    case 't':
      out += '\t';
      break;
    case 'r':
      break;
    case 'b':
    case 'f':
      break;
    case 'u': {
      if (i + 4 > json.size())
        return out;
      uint32_t cp = std::stoul(json.substr(i, 4), nullptr, 16);
      i += 4;
      if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= json.size() &&
          json[i] == '\\' && json[i + 1] == 'u') {
        uint32_t lo = std::stoul(json.substr(i + 2, 4), nullptr, 16);
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 6;
      }
      append_utf8(out, cp);
      break;
    }
    default:
      out += e;
      break; // \" \\ \/
    }
  }
  return out;
}

struct RequestCancelled : std::runtime_error {
  RequestCancelled() : std::runtime_error("request cancelled") {}
};

class OllamaClient {
public:
  OllamaClient(const std::string& url = "http://localhost:11434/api/chat")
      : apiUrl(url) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  }

  ~OllamaClient() { curl_global_cleanup(); }

  // Streams the Russian translation of `text`: a sentence as HTML with the
  // clicked word in <b>, which the model often keeps around its translation,
  // or a single word. The prompt is translategemma's own (plus one line on
  // tenses): the model is trained on it and needs no examples. Short matters:
  // the model runs on the CPU and ollama does not reuse the cached prefix for
  // gemma3 models, so ~90 prompt
  // tokens take ~2 s before the first word, the old prompt with examples
  // (~400) took ~8 s. Re-casing ALL CAPS text is up to the caller
  // (sentence_case), before the tags are added.
  // on_chunk is called with each new piece as it is generated.
  // Thread-safe: every call uses its own CURL handle.
  // If `cancelled` returns true the connection is dropped, which also stops
  // generation in ollama.
  // `previous`: translations of `text` already shown; the model is asked for
  // another one in a dialog, like "Переведи по-другому" of Yandex. Temperature
  // alone gives the same answer almost every time.
  void translate(const std::string& text,
                 const std::function<void(const std::string&)>& on_chunk,
                 std::function<bool()> cancelled = {},
                 double temperature = 0,
                 const std::vector<std::string>& previous = {}) const {
    std::string messages = message("user", prompt + text);
    for (const auto& t : previous)
      messages += "," + message("assistant", t) + "," +
                  message("user", "Переведи по-другому.");
    chat(messages, on_chunk, std::move(cancelled), temperature);
  }

  // Words of `translation` that translate `word` of `text`, for when the model
  // dropped the **…** around them: "they **sorely** lack" -> "им так не
  // хватает" -> "так не хватает". May be the whole sentence or wrong words.
  std::string aligned(const std::string& text, const std::string& translation,
                      const std::string& word,
                      std::function<bool()> cancelled = {}) const {
    std::string out;
    chat(message("user", prompt + text) + "," +
             message("assistant", translation) + "," +
             message("user", "Which words of your translation translate \"" +
                                 word +
                                 "\"? Answer with those Russian words only."),
         [&](const std::string& piece) { out += piece; },
         std::move(cancelled), 0);
    return out;
  }

  // Meaning of `word` in `text` in simple words, and its style, for when the
  // dictionaries have no simple definition (see Lexicon::apply_model_answer):
  // "DEFINITION: ...\nSTYLE: Formal". The translation model follows such
  // requests well enough for the definition; its CEFR levels and word parts
  // are guesses, those come from the dictionaries only. ~6-10 s on the CPU.
  std::string explain(const std::string& word, const std::string& pos,
                      const std::string& text,
                      std::function<bool()> cancelled = {}) const {
    std::string out;
    chat(message("user",
                 "Explain the English " + (pos.empty() ? "word" : pos) +
                     " \"" + word + "\" as it is used in this text: \"" +
                     text +
                     "\"\nAnswer in English, in exactly these lines:\n"
                     "DEFINITION: <a learner's dictionary definition of the "
                     "word in the meaning it has here, not a retelling of the "
                     "text: one short sentence of very simple, common words, "
                     "without the word itself>\n"
                     "STYLE: <Neutral, Formal, Informal, Slang, Technical, "
                     "Literary or Old-fashioned>\n"
                     // without an example it retells the sentence: "They
                     // sorely lack experience" -> "They don't have enough
                     // experience"
                     "For example, for \"swiftly\" in \"She swiftly left "
                     "the room\":\nDEFINITION: In a quick way.\nSTYLE: "
                     "Neutral"),
         [&](const std::string& piece) { out += piece; },
         std::move(cancelled), 0);
    return out;
  }

  // Loads the model into memory and keeps it there, so the first real request
  // is fast
  void warmup() const {
    post("http://localhost:11434/api/generate",
         R"({"model":")" + model + R"(","keep_alive":-1})", {}, {});
  }

private:
  static std::string to_lower(std::string s) {
    for (char& c : s)
      c = std::tolower(static_cast<unsigned char>(c));
    return s;
  }

public:
  // Comics are written in caps, which small models translate much worse:
  // "I WILL NOW IMPART THE ARTS." -> "I will now impart the arts."
  static std::string sentence_case(const std::string& s) {
    bool has_lower = false, has_upper = false;
    for (unsigned char c : s) {
      has_lower |= std::islower(c) != 0;
      has_upper |= std::isupper(c) != 0;
    }
    if (has_lower || !has_upper)
      return s;

    std::string out = to_lower(s);
    bool sentence_start = true;
    for (size_t i = 0; i < out.size(); ++i) {
      unsigned char c = out[i];
      if (std::isalpha(c)) {
        const bool word_start = i == 0 || !std::isalpha((unsigned char)out[i - 1]);
        const bool lone_i =
            c == 'i' && word_start &&
            (i + 1 == out.size() || !std::isalpha((unsigned char)out[i + 1]));
        if (sentence_start || lone_i)
          out[i] = std::toupper(c);
        sentence_start = false;
      } else if (c == '.' || c == '!' || c == '?') {
        sentence_start = true;
      }
    }
    return out;
  }

private:

  static std::string message(const char* role, const std::string& content) {
    return R"({"role":")" + std::string(role) + R"(","content":")" +
           escape_json(content) + "\"}";
  }

  // `messages` is a comma-separated list of message() objects
  void chat(const std::string& messages,
            const std::function<void(const std::string&)>& on_chunk,
            std::function<bool()> cancelled, double temperature) const {
    std::string json = R"({"model":")" + model +
                       R"(","stream":true,"keep_alive":-1,)"
                       R"("options":{"temperature":)" +
                       // not std::to_string: Qt sets the locale, "0,8"
                       std::to_string(int(temperature)) + "." +
                       std::to_string(int(temperature * 10) % 10) +
                       R"(,"num_predict":200},"messages":[)" + messages + "]}";

    // ollama streams one JSON object per line:
    // {"message":{"role":"assistant","content":"<piece>"},"done":false}
    std::string pending, error;
    DataCallback on_data = [&](const char* data, size_t n) {
      pending.append(data, n);
      size_t nl;
      while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);

        error = json_string_field(line, "error");
        if (!error.empty())
          return false;

        std::string piece = json_string_field(line, "content");
        if (!piece.empty())
          on_chunk(piece);
      }
      return true;
    };
    try {
      post(apiUrl, json, on_data, std::move(cancelled));
    } catch (const RequestCancelled&) {
      throw;
    } catch (const std::exception&) {
      if (!error.empty())
        throw std::runtime_error("ollama: " + error);
      throw;
    }
  }

  static int ProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t,
                              curl_off_t) {
    auto* cancelled = static_cast<std::function<bool()>*>(clientp);
    return (*cancelled && (*cancelled)()) ? 1 : 0;
  }

  static void post(const std::string& url, const std::string& jsonData,
                   DataCallback on_data, std::function<bool()> cancelled) {
    std::unique_ptr<CURL, void (*)(CURL*)> curl(curl_easy_init(),
                                                curl_easy_cleanup);
    if (!curl) {
      throw std::runtime_error("Ошибка инициализации CURL");
    }
    std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(
        curl_slist_append(nullptr, "Content-Type: application/json"),
        curl_slist_free_all);

    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, jsonData.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &on_data);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl.get());
    if (res == CURLE_ABORTED_BY_CALLBACK) {
      throw RequestCancelled();
    }
    if (res != CURLE_OK) {
      throw std::runtime_error("Ошибка запроса CURL: " +
                               std::string(curl_easy_strerror(res)));
    }
  }

  std::string apiUrl;
  std::string model = "translategemma:4b"; // qwen2.5:3b, gemma3:1b, gemma3:4b qwen3:1.7b
  // translategemma's prompt format, the text goes right after it
  std::string prompt =
      "You are a professional English (en) to Russian (ru) translator. Your "
      "goal is to accurately convey the meaning and nuances of the original "
      "English text while adhering to Russian grammar, vocabulary, and "
      "cultural sensitivities.\n"
      // "Sorry for messaging so late" came out as "...что ответил так поздно"
      "Keep the tense of the original: an -ing form happens at the same time "
      "as the main verb unless the text says otherwise (\"Sorry for calling so "
      "late\" is \"Извините, что звоню так поздно\").\n"
      // "Max 5x users" came out as "Максимум 5 пользователям"
      "Keep product, plan and brand names as they are (\"Max 5x users\" is "
      "\"пользователи Max 5x\").\n"
      "Produce only the Russian translation, without any additional "
      "explanations or commentary. Please translate the following English "
      "text into Russian:\n\n\n";
};

#endif // __OLLAMA_CLIENT__
