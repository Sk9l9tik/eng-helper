#ifndef __YANDEX_TRANSLATE__
#define __YANDEX_TRANSLATE__

#include <curl/curl.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "LLM.h" // escape_json, json_string_field

// Yandex Cloud Translate API v2. Configured from the environment:
//   YANDEX_TRANSLATE_API_KEY  API key of a service account, or
//   YANDEX_IAM_TOKEN          IAM token (then YANDEX_FOLDER_ID is required)
//   YANDEX_FOLDER_ID          folder of the service, optional with an API key
// Without them configured() is false and the local model translates.
class YandexTranslator {
public:
  YandexTranslator() {
    if (const char* key = std::getenv("YANDEX_TRANSLATE_API_KEY"); key && *key)
      auth_ = std::string("Authorization: Api-Key ") + key;
    else if (const char* iam = std::getenv("YANDEX_IAM_TOKEN"); iam && *iam)
      auth_ = std::string("Authorization: Bearer ") + iam;
    if (const char* folder = std::getenv("YANDEX_FOLDER_ID"); folder && *folder)
      folder_ = folder;
  }

  bool configured() const { return !auth_.empty(); }

  // English HTML -> Russian HTML. Tags are carried over to the words they
  // wrap: "I will <b>impart</b> the arts" -> "Я <b>передам</b> искусства", so
  // the translation of a word is found exactly. Thread-safe.
  std::string translate_html(const std::string& html) const {
    std::string body =
        R"({"sourceLanguageCode":"en","targetLanguageCode":"ru",)"
        R"("format":"HTML","texts":[")" +
        escape_json(html) + "\"]";
    if (!folder_.empty())
      body += R"(,"folderId":")" + escape_json(folder_) + "\"";
    body += "}";

    long status = 0;
    const std::string response = post(body, status);
    if (status != 200) {
      std::string message = json_string_field(response, "message");
      throw std::runtime_error("Yandex Translate: " +
                               (message.empty() ? "HTTP " +
                                                      std::to_string(status)
                                                : message));
    }
    // {"translations":[{"text":"...","detectedLanguageCode":"en"}]}
    return json_string_field(response, "text");
  }

private:
  static size_t write(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
  }

  std::string post(const std::string& body, long& status) const {
    std::unique_ptr<CURL, void (*)(CURL*)> curl(curl_easy_init(),
                                                curl_easy_cleanup);
    if (!curl)
      throw std::runtime_error("Ошибка инициализации CURL");
    curl_slist* list = curl_slist_append(nullptr, "Content-Type: application/json");
    list = curl_slist_append(list, auth_.c_str());
    std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(
        list, curl_slist_free_all);

    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL,
                     "https://translate.api.cloud.yandex.net/translate/v2/"
                     "translate");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl.get());
    if (res != CURLE_OK)
      throw std::runtime_error("Yandex Translate: " +
                               std::string(curl_easy_strerror(res)));
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return response;
  }

  std::string auth_, folder_;
};

#endif // __YANDEX_TRANSLATE__
