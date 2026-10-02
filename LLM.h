#ifndef __OLLAMA_CLIENT__
#define __OLLAMA_CLIENT__

#include <cstdint>
#include <curl/curl.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

  // Streams two lines: Russian translation of `word` with the meaning it has in
  // `sentence`, then of the whole sentence. `options` are the dictionary
  // translations of the word ("банк; берег"): small models pick the right
  // meaning from a list much better than they come up with it. May be empty.
  // on_chunk is called with each new piece as it is generated.
  // Thread-safe: every call uses its own CURL handle.
  // If `cancelled` returns true the connection is dropped, which also stops
  // generation in ollama.
  void translate(const std::string& word, const std::string& sentence,
                 const std::string& options,
                 const std::function<void(const std::string&)>& on_chunk,
                 std::function<bool()> cancelled = {}) const {
    chat(message("system", system_prompt) + "," +
             message("user", "Sentence: I sat on the river bank.\nWord: "
                             "bank\nDictionary: банк; берег; насыпь") +
             "," + message("assistant", "берег\nЯ сидел на берегу реки.") +
             "," +
             message("user", "Sentence: Do you still live there?\nWord: "
                             "still\nDictionary: неподвижный; ещё, всё ещё") +
             "," + message("assistant", "всё ещё\nТы всё ещё там живёшь?") +
             "," +
             message("user",
                     "Sentence: " + sentence + "\nWord: " + word +
                         "\nDictionary: " + (options.empty() ? "-" : options)),
         on_chunk, std::move(cancelled));
  }

  // Streams only the Russian translation of `sentence`.
  // Fallback for translate(): small models sometimes stop after the first line.
  void
  translate_sentence(const std::string& sentence,
                     const std::function<void(const std::string&)>& on_chunk,
                     std::function<bool()> cancelled = {}) const {
    chat(message("system", sentence_prompt) + "," +
             message("user", "I sat on the river bank.") + "," +
             message("assistant", "Я сидел на берегу реки.") + "," +
             message("user", sentence),
         on_chunk, std::move(cancelled));
  }

  // Loads the model into memory and keeps it there, so the first real request
  // is fast
  void warmup() const {
    post("http://localhost:11434/api/generate",
         R"({"model":")" + model + R"(","keep_alive":-1})", {}, {});
  }

private:
  static std::string message(const char* role, const std::string& content) {
    return R"({"role":")" + std::string(role) + R"(","content":")" +
           escape_json(content) + "\"}";
  }

  // `messages` is a comma-separated list of message() objects
  void chat(const std::string& messages,
            const std::function<void(const std::string&)>& on_chunk,
            std::function<bool()> cancelled) const {
    std::string json =
        R"({"model":")" + model +
        R"(","stream":true,"keep_alive":-1,)"
        R"("options":{"temperature":0,"num_predict":200},"messages":[)" +
        messages + "]}";

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
  std::string model = "gemma3:1b"; // qwen2.5:3b, gemma3:1b, gemma3:4b qwen3:1.7b
  // Short prompt on purpose: gemma3 does not reuse the cached prefix, so every
  // prompt token costs time on each request
  std::string system_prompt =
      "You are an English-Russian translator. The user gives a sentence, a "
      "word from it and dictionary translations of the word. Reply with "
      "exactly two lines:\n"
      "1) the Russian translation of the word as it is used in this sentence, "
      "1-3 words, in dictionary form; prefer a dictionary option if one fits\n"
      "2) a natural, fluent Russian translation of the whole sentence\n"
      "No labels, no quotes, no other text.";
  std::string sentence_prompt =
      "You are an English-Russian translator. Translate the user's English "
      "sentence into natural, fluent Russian. "
      "Reply with the translation only.";
};

#endif // __OLLAMA_CLIENT__
