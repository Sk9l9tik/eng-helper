#include <iostream>
#include <string>
#include <curl/curl.h>


static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static std::string escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

class OllamaClient {
public:
    OllamaClient(const std::string& url = "http://localhost:11434/v1/completions") 
        : apiUrl(url) 
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Ошибка инициализации CURL"); }
        headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    }

    ~OllamaClient() {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
    }

    std::string sendRequest(const std::string& prompt) {
        std::string readBuffer;
        std::string tmp = escape_json(setup + prompt);
        std::string jsonData = R"({"model":"gemma3:1b","prompt":")" + tmp + "\"}";

        curl_easy_setopt(curl, CURLOPT_URL, apiUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            throw std::runtime_error("Ошибка запроса CURL: " + std::string(curl_easy_strerror(res)));
        }

        return readBuffer;
    }

    std::string get_setup() {
        return setup;
    }

private:
    CURL* curl;
    struct curl_slist* headers;
    std::string apiUrl;
    std::string setup = R"(
You will receive an input string in the form "<word>$<context>" (English). Your task is to produce a single concise response in exactly the following format (without any additional text, commentary, labels or punctuation): <Definition of <word> in English>/<Translation of <context> into Russian>Rules:

1. The first part (before the grid "#") must be a brief definition of the word in English (one sentence, clear and directly describing the meaning relevant to the provided context when possible).
2. The second part (after the grid "#") MUST be the translation of the context into RUSSIAN (natural, grammatical RUSSIAN). If the input contains only a word with no context (i.e. no "$" or nothing after "$"), then the second part must be the Russian translation of the word itself.
3. The "#" character separates the two parts and indicates a line break; produce exactly one "#" between the two parts. Do not produce a literal newline—use the "/" as the separator as requested.
4. Do not add anything else: no headings, no examples, no extra sentences, no punctuation outside the two parts except the single separating "/".
5. Preserve the input word unchanged in spelling/case when producing the definition (you may use it inside the definition).
6. Keep the definition concise (preferably ≤ 20 words) and the translation faithful to the given context.
7. If the context is ambiguous, produce the most common/general interpretation; still follow rules above and do not ask clarifying questions.

Examples (these are examples of the required output format only):
Input: "apple$fruit from a tree"
Output: "A round sweet fruit produced by an apple tree#фрукт круглой формы, растущий на яблоне"
Input: "serendipity$"
Output: "The occurrence of pleasant unexpected discoveries#удивительная, приятная случайность"

Now process the incoming input (format "<word>$<context>") and produce the single-line output exactly in the required format.
Input: 
)"; 

    // std::string setup = "At the end of the text there will be a message: in English in this format: <word>:<context>. You need to give an answer in this format: Definition:<Definition of a <word> in English>/Translation:<Translation of <context> into Russian>. The '/' sign indicates a line break. YOU ONLY NEED TO GIVE AN ANSWER IN THE FORMAT THAT I WROTE: <Definition of a word in English>/<Translate context into Russian>, AND DON'T WRITE ANYTHING ELSE. If I only wrote a word, but there is no context, then you need to give a translation of the word instead of the context translation. Now answer this:";
};

