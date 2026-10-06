#ifndef __TESSERACT_PARSER__
#define __TESSERACT_PARSER__

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <leptonica/allheaders.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <tesseract/baseapi.h>
#include <tesseract/publictypes.h>
#include <tesseract/resultiterator.h>

#include "Text.h"

// TODO: wrap this code into namespace

// How Parser::process recognizes an image
struct OcrOptions {
  double scale = 0; // 0: 2x for a part of the screen, 1x for the whole
  double shear = 0; // > 0 straightens italics leaning right
};

class Parser {
public:
  // Loading a model takes ~150 ms, so all instances are loaded in parallel.
  // Parser is created before the screenshot is taken (see main), so loading
  // overlaps with the capture.
  explicit Parser(const std::string& tesseract_data_path = "",
                  const std::string& lang = "eng") {
    std::vector<std::future<TesseractApi>> apis;
    for (int i = 0; i < worker_count(); ++i)
      apis.push_back(
          std::async(std::launch::async, make_api, tesseract_data_path, lang));
    for (auto& a : apis)
      apis_.push_back(a.get());
  }

  const std::vector<Block>& get_blocks() const { return blocks_; }
  const std::vector<Word>& get_words() const { return words_; }

  void save_detected_words(const std::string& filename) const {
    ::save_detected_words(filename, blocks_, words_);
  }

  // The image is split into horizontal strips recognized in parallel, one per
  // instance: tesseract barely scales with OpenMP threads. Almost all the time
  // is LSTM recognition, proportional to the amount of text; more strips than
  // instances (for balance) and other page segmentation modes were no faster.
  // `offset` is where `image` is on the screen: word boxes are in screen
  // coordinates. Words cut by the image edge are dropped: `image` is usually
  // a part of the screen.
  using Options = OcrOptions;
  void process(const cv::Mat& image, cv::Point offset = {},
               Options options = {}) {
    if (image.empty())
      throw std::runtime_error("Empty image passed to Parser");

    cv::Mat gray;
    if (image.channels() == 4)
      cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else if (image.channels() == 3)
      cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else if (image.channels() == 1)
      gray = image;
    else
      throw std::runtime_error("Unsupported number of channels in image");
    // small text (~7 px x-height, a chat or a caption) is mostly lost at screen
    // size; a part of the screen is cheap to recognize at 2x, the whole one
    // is not
    const double scale = options.scale > 0                    ? options.scale
                         : gray.total() <= kMaxUpscaledArea ? 2.0
                                                            : 1.0;
    if (scale != 1.0)
      cv::resize(gray, gray, cv::Size(), scale, scale, cv::INTER_CUBIC);

    cv::Mat norm = dark_text_on_light(gray);
    // x' = x + shear * y: lower rows move right, so a letter leaning right
    // stands upright
    const double shear = options.shear;
    if (shear > 0) {
      const cv::Matx23d m(1.0, shear, 0.0, 0.0, 1.0, 0.0);
      cv::warpAffine(norm, norm, m,
                     cv::Size(norm.cols + static_cast<int>(shear * norm.rows),
                              norm.rows),
                     cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(255));
    }
    img_size_ = norm.size();
    // a strip much lower than the overlap recognizes the same lines several
    // times: a small image gets fewer strips
    constexpr int min_strip = 150;
    const std::vector<int> cuts = strip_cuts(
        norm, std::clamp(norm.rows / min_strip, 1,
                         static_cast<int>(apis_.size())));
    // A cut can still go through a line (a video frame has no empty rows), so
    // strips overlap by `overlap` rows and each keeps only words centered in
    // its own rows: every line up to 2 * `overlap` high is seen whole once.
    // More overlap costs time: at 100 recognition was ~50% slower.
    constexpr int overlap = 40;
    std::vector<Strip> strips;
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
      const int y0 = std::max(0, cuts[k] - overlap),
                y1 = std::min(norm.rows, cuts[k + 1] + overlap);
      strips.push_back({norm(cv::Range(y0, y1), cv::Range::all()),
                        y0,
                        cuts[k],
                        cuts[k + 1],
                        {},
                        {}});
    }

    std::atomic<size_t> next{0};
    std::vector<std::future<void>> jobs;
    for (auto& api : apis_)
      jobs.push_back(std::async(std::launch::async, [&, api = api.get()] {
        for (size_t k; (k = next++) < strips.size();) {
          recognize(*api, strips[k].img);
          extract_blocks(*api, strips[k]);
          extract_words_and_map_to_blocks(*api, strips[k]);
        }
      }));
    for (auto& j : jobs)
      j.get();

    // strips have their own block and paragraph ids; paragraphs of different
    // strips never merge
    blocks_.clear();
    words_.clear();
    int para_offset = 0;
    for (auto& st : strips) {
      const int block_offset = static_cast<int>(blocks_.size());
      int para_count = 0;
      for (auto& b : st.blocks) {
        b.id += block_offset;
        to_screen(b.box, offset, scale, shear);
        blocks_.push_back(std::move(b));
      }
      for (auto& w : st.words) {
        if (touches_edge(w.box))
          continue;
        if (w.block_id >= 0)
          w.block_id += block_offset;
        para_count = std::max(para_count, w.para_id + 1);
        to_screen(w.box, offset, scale, shear);
        to_screen(w.line, offset, scale, shear);
        w.para_id += para_offset;
        words_.push_back(std::move(w));
      }
      para_offset += para_count;
    }
    fix_misread_i(words_);
    build_contexts(words_);
  }

  // Screens mix dark text on light (a white banner, a link preview) and light
  // text on dark (a dark chat). Text is a minority of pixels around it, so a
  // large median gives the local background, and the distance to it gives text
  // of either polarity as dark on light. The median runs on a downscaled copy:
  // full size is slow.
  static cv::Mat dark_text_on_light(const cv::Mat& gray) {
    constexpr int scale = 4;
    constexpr int kernel = 25; // ~100 px in the full image: wider than a glyph,
                               // narrower than a panel
    cv::Mat small, bg;
    cv::resize(gray, small, cv::Size(), 1.0 / scale, 1.0 / scale,
               cv::INTER_AREA);
    cv::medianBlur(small, small, kernel);
    cv::resize(small, bg, gray.size(), 0, 0, cv::INTER_LINEAR);
    cv::Mat diff, out;
    cv::absdiff(gray, bg, diff);
    cv::subtract(cv::Scalar(255), diff, out);
    return out;
  }

private:
  using TesseractApi = std::unique_ptr<tesseract::TessBaseAPI,
                                       void (*)(tesseract::TessBaseAPI*)>;

  static TesseractApi make_api(const std::string& tesseract_data_path,
                               const std::string& lang) {
    TesseractApi api(new tesseract::TessBaseAPI(),
                     [](tesseract::TessBaseAPI* p) {
                       if (p) {
                         p->End();
                         delete p;
                       }
                     });
    if (api->Init(tesseract_data_path.empty() ? NULL
                                              : tesseract_data_path.c_str(),
                  lang.c_str()))
      throw std::runtime_error("Failed to initialize tesseract API");
    // text is always given dark on light (see dark_text_on_light), so
    // tesseract's own inverted-text pass is not needed
    api->SetVariable("tessedit_do_invert", "0");
    return api;
  }

  static constexpr int kMaxUpscaledArea = 1000 * 1000;

  static void to_screen(Rect& r, cv::Point offset, double scale,
                        double shear) {
    // the shift of the box's middle row undone
    const int dx = static_cast<int>(shear * (r.y1 + r.y2) / 2);
    r.x1 = static_cast<int>((r.x1 - dx) / scale) + offset.x;
    r.x2 = static_cast<int>((r.x2 - dx) / scale) + offset.x;
    r.y1 = static_cast<int>(r.y1 / scale) + offset.y;
    r.y2 = static_cast<int>(r.y2 / scale) + offset.y;
  }

  bool touches_edge(const Rect& r) const {
    constexpr int margin = 2;
    return r.x1 <= margin || r.y1 <= margin ||
           r.x2 >= img_size_.width - 1 - margin ||
           r.y2 >= img_size_.height - 1 - margin;
  }

  struct Strip {
    cv::Mat img;        // view into the normalized image
    int y0;             // strip offset in the full image
    int own_y1, own_y2; // rows of the full image this strip is responsible for,
                        // the rest is overlap
    std::vector<Block> blocks; // ids from 0 within the strip
    std::vector<Word> words;   // block and paragraph ids within the strip

    // y1, y2 are within the strip image
    bool owns(int y1, int y2) const {
      const int cy = y0 + (y1 + y2) / 2;
      return cy >= own_y1 && cy < own_y2;
    }
  };

  // one tesseract instance per core
  static int worker_count() {
    return std::max(1u, std::thread::hardware_concurrency());
  }

  // Rows where the image is cut into n strips. A cut is placed on the most
  // uniform row near the even split, so it goes between text lines rather than
  // through them.
  static std::vector<int> strip_cuts(const cv::Mat& gray, int n) {
    std::vector<int> cuts{0};
    const int window = gray.rows / (4 * n);
    for (int k = 1; k < n; ++k) {
      const int target = gray.rows * k / n;
      int best = target;
      double best_dev = std::numeric_limits<double>::max();
      for (int y = std::max(cuts.back() + 1, target - window);
           y <= std::min(gray.rows - 1, target + window); ++y) {
        cv::Scalar mean, dev;
        cv::meanStdDev(gray.row(y), mean, dev);
        // ties go to the row closest to the even split
        if (dev[0] < best_dev ||
            (dev[0] == best_dev &&
             std::abs(y - target) < std::abs(best - target))) {
          best_dev = dev[0];
          best = y;
        }
      }
      cuts.push_back(best);
    }
    cuts.push_back(gray.rows);
    return cuts;
  }

  static void recognize(tesseract::TessBaseAPI& api, const cv::Mat& img) {
    api.SetImage(img.data, img.cols, img.rows, img.channels(),
                 static_cast<int>(img.step));
    api.SetPageSegMode(tesseract::PSM_AUTO);
    if (api.Recognize(0) != 0)
      throw std::runtime_error("Tesseract recognize failed");
  }

  using TesseractString = std::unique_ptr<char, void (*)(void*)>;
  static TesseractString make_text(char* p) {
    return TesseractString(p,
                           [](void* mem) { delete[] static_cast<char*>(mem); });
  }

  static void extract_blocks(tesseract::TessBaseAPI& api, Strip& strip) {
    std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());
    if (!it)
      return;

    tesseract::PageIteratorLevel level = tesseract::RIL_TEXTLINE;
    int block_id = 0;

    do {
      auto block_text = make_text(it->GetUTF8Text(level));

      if (block_text) {
        int x1, y1, x2, y2;
        if (it->BoundingBox(level, &x1, &y1, &x2, &y2) && strip.owns(y1, y2)) {
          Block b;
          b.id = block_id++;
          b.box = Rect(x1, y1 + strip.y0, x2, y2 + strip.y0);
          b.text = block_text ? std::string(block_text.get()) : "";
          b.words = [&b]() {
            std::vector<std::string> v;
            std::string s = b.text;
            std::istringstream ss(s);
            std::string tmp;
            while (ss >> tmp) {
              v.emplace_back(tmp);
            }
            return v;
          }();
          strip.blocks.emplace_back(b);
        }
      }

    } while (it->Next(level));
  }

  void extract_words_and_map_to_blocks(tesseract::TessBaseAPI& api,
                                       Strip& strip) const {
    std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());
    if (!it)
      return;

    tesseract::PageIteratorLevel level = tesseract::RIL_WORD;
    int para_id = -1;

    do {
      if (it->IsAtBeginningOf(tesseract::RIL_PARA))
        ++para_id;

      auto word_text = make_text(it->GetUTF8Text(level));

      float conf = it->Confidence(level);

      int x1, y1, x2, y2;
      bool has_box = it->BoundingBox(level, &x1, &y1, &x2, &y2);

      // READ: This check is real need?
      if (!has_box) {
        if (word_text)
          word_text.get_deleter();
        continue;
      }

      // low confidence words are mostly icons, borders and other noise
      if (!word_text || conf < 50.0f)
        continue;

      // tesseract sometimes returns whitespace or lines as "words" with huge
      // boxes over images and panels
      std::string_view text_view(word_text.get());
      // "|" is kept for now: it may be a misread "I" (see fix_misread_i)
      if (text_view != "|" &&
          std::none_of(text_view.begin(), text_view.end(), [](unsigned char c) {
            return std::isalnum(c) || c >= 0x80;
          }))
        continue;
      if (y2 - y1 > img_size_.height / 4 || x2 - x1 > img_size_.width / 2)
        continue;
      // in the overlap: the neighbour strip has it
      if (!strip.owns(y1, y2))
        continue;

      Word w;
      w.text = word_text ? std::string(word_text.get()) : "";
      // w.box = Rect(x1-3 , y1-3, x2 + 5, y2 + 7); // simple shift for best
      // rendering
      w.box = Rect(x1, y1 + strip.y0, x2, y2 + strip.y0);
      w.confidence = conf;
      w.block_id = find_block_for_word(w, strip.blocks);
      w.para_id = para_id;
      if (it->BoundingBox(tesseract::RIL_TEXTLINE, &w.line.x1, &w.line.y1,
                          &w.line.x2, &w.line.y2)) {
        w.line.y1 += strip.y0;
        w.line.y2 += strip.y0;
      } else
        w.line = w.box;

      strip.words.emplace_back(std::move(w));

      if (word_text)
        word_text.get_deleter();
    } while (it->Next(level));
  }

  static int find_block_for_word(const Word& w,
                                 const std::vector<Block>& blocks) {
    double cx = (w.box.x1 + w.box.x2) / 2.0;
    double cy = (w.box.y1 + w.box.y2) / 2.0;

    for (const auto& b : blocks)
      if (cx >= b.box.x1 && cx <= b.box.x2 && cy >= b.box.y1 && cy <= b.box.y2)
        return b.id;

    // for(const auto& wd : b.words)
    //     if (w.text == wd)
    // if (b.text.find(w.text) != std::string::npos)

    return -1;
  }

private:
  std::vector<TesseractApi> apis_;
  cv::Size img_size_; // of the last processed image

  std::vector<Block> blocks_;
  std::vector<Word> words_;
};

#endif //__TESSERACT_PARSER__
