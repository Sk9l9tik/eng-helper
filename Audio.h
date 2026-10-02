#ifndef __PRONUNCIATIONS__
#define __PRONUNCIATIONS__

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <curl/curl.h>

// Word pronunciations from Wiktionary. index.tsv ("word\tmp3 url") is built by
// tools/fetch_dictionaries.py, mp3 files are downloaded next to it on first
// use.
class Pronunciations {
public:
  explicit Pronunciations(std::string dir) : dir_(std::move(dir)) {}

  // Path of the mp3 for `word`, downloaded if needed; empty if there is no
  // recording. Thread-safe.
  std::string fetch(const std::string& word) {
    std::call_once(loaded_, [this] { load_index(); });
    auto it = urls_.find(word);
    if (it == urls_.end())
      return "";

    std::string path = dir_ + "/" + file_name(word);
    if (std::filesystem::exists(path))
      return path;
    return download(it->second, path) ? path : "";
  }

  bool has_index() const {
    return std::filesystem::exists(dir_ + "/index.tsv");
  }

private:
  void load_index() {
    std::ifstream f(dir_ + "/index.tsv");
    std::string line;
    while (std::getline(f, line)) {
      size_t tab = line.find('\t');
      if (tab != std::string::npos)
        urls_.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
  }

  // same as audio_file() in tools/fetch_dictionaries.py
  static std::string file_name(std::string word) {
    for (auto& c : word)
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'' ||
            c == '-'))
        c = '_';
    return word + ".mp3";
  }

  static size_t write_file(void* data, size_t size, size_t n, void* file) {
    return fwrite(data, size, n, static_cast<FILE*>(file)) * size;
  }

  static bool download(const std::string& url, const std::string& path) {
    const std::string tmp = path + ".part";
    std::unique_ptr<FILE, int (*)(FILE*)> file(fopen(tmp.c_str(), "wb"),
                                               fclose);
    if (!file)
      return false;

    std::unique_ptr<CURL, void (*)(CURL*)> curl(curl_easy_init(),
                                                curl_easy_cleanup);
    if (!curl)
      return false;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    // Wikimedia rejects requests without a User-Agent
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT,
                     "LookUpper/1.0 (https://github.com/Sk9l9tik/eng-helper)");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, file.get());
    const bool ok = curl_easy_perform(curl.get()) == CURLE_OK;
    file.reset();

    std::error_code ec;
    if (ok)
      std::filesystem::rename(tmp, path, ec);
    else
      std::filesystem::remove(tmp, ec);
    return ok && !ec;
  }

  std::string dir_;
  std::once_flag loaded_;
  std::unordered_map<std::string, std::string> urls_;
};

#endif // __PRONUNCIATIONS__
