#ifndef __TESSERACT_PARSER__
#define __TESSERACT_PARSER__

#include <iostream>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <fstream>
#include <string>

#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <tesseract/publictypes.h>
#include <tesseract/resultiterator.h>

// TODO: wrap to namespace 

struct Rect {
    int x1, y1, x2, y2;

    Rect(int x1, int y1, int x2, int y2) : x1(x1), y1(y1), x2(x2), y2(y2) {}
    Rect() : x1(0), y1(0), x2(0), y2(0) {}
};

struct Block {
   int id;
   std::string text;
   Rect box;

   Block(int id_, std::string text_, Rect box_) : id(id_), text(text_), box(box_) {};
   Block() : id(0), text(""), box({}) {};
};

struct Word{
    std::string text;
    Rect box;
    int block_id;
    float confidence;

   // Word(int id_, std::string text_, Rect box_) : id(id_), text(text_), box(box_) {};
   // Word() : id(0), text(""), box({}) {};
};

class Parser {
public:
    explicit Parser(const cv::Mat& image, const std::string& tesseract_data_path = "", const std::string& lang = "eng")
        : img_original_(image), api_(nullptr, [](tesseract::TessBaseAPI* p){ if (p) { p->End(); delete p;} }) {
            if (img_original_.empty())
                throw std::runtime_error("Empty image passed to Parser");
            
            tesseract::TessBaseAPI* raw = new tesseract::TessBaseAPI();
            if (raw->Init(tesseract_data_path.empty() ? NULL : tesseract_data_path.c_str(), lang.c_str())){
                delete raw;
                throw std::runtime_error("Failed to initialize tesseract API");
            }
            api_.reset(raw);

            if (img_original_.channels() == 4) {
                cv::cvtColor(img_original_, img_rgb_, cv::COLOR_BGRA2RGB);
            }
            else if (img_original_.channels() == 3) {
                cv::cvtColor(img_original_, img_rgb_, cv::COLOR_BGR2RGB);
            }
            else if (img_original_.channels() == 1) {
                cv::cvtColor(img_original_, img_rgb_, cv::COLOR_GRAY2RGB);
            }
            else {
                throw std::runtime_error("Unsupported number of channels in image");
            }
        }

    const std::vector<Block>& get_blocks() const { return blocks_; }
    const std::vector<Word>& get_words() const { return words_; }

    // start pipeline
    void process(){
        set_image_to_api();
        recognize();
        extract_blocks();
        extract_words_and_map_to_blocks();
    }


    void save_detected_words(const std::string& filename) const {
        std::ofstream ofs(filename);
        if(!ofs) throw std::runtime_error("Cannot open output file");


        ofs << "Blocks:\n";
        for (const auto& b : blocks_) {
            ofs << b.id << ": (" << b.box.x1 << "," << b.box.y1 << ") - (" << b.box.x2 << "," << b.box.y2 << ")\n";
            ofs << b.text << "\n---\n";
        }


        ofs << "\nWords:\n";
        for (const auto& w : words_) {
            ofs << "block_id=" << w.block_id << " conf=" << w.confidence << " text=\"" << w.text << "\"\n";
            ofs << w.box.x1 << " " << w.box.y1 << "\n";
            ofs << w.box.x2 << " " << w.box.y2 << "\n\n";
        }
    }
private:
    void set_image_to_api(){
        api_->SetImage(img_rgb_.data, img_rgb_.cols, img_rgb_.rows, img_rgb_.channels(), static_cast<int>(img_rgb_.step));
    }

    void recognize(){
        api_->SetPageSegMode(tesseract::PSM_AUTO);
        if(api_->Recognize(0) != 0)
            throw std::runtime_error("Tesseract recognize failed");
    }


    using TesseractString = std::unique_ptr<char, void(*)(void*)>;
    TesseractString make_text(char* p) {
        return TesseractString(p, [](void* mem){ delete[] static_cast<char*>(mem); });
    }

    void extract_blocks(){
        blocks_.clear();
        std::unique_ptr<tesseract::ResultIterator> it(api_->GetIterator());
        if (!it) return;

        tesseract::PageIteratorLevel level = tesseract::RIL_PARA;
        int block_id{};

        do {
            auto block_text = make_text(it->GetUTF8Text(level));

            if (block_text){
                int x1, y1, x2, y2;
                if (it->BoundingBox(level, &x1, &y1, &x2, &y2)) {
                    Block b;
                    b.id = block_id++;
                    b.box = Rect(x1, y1, x2, y2);
                    b.text = block_text ? std::string(block_text.get()) : "";
                    blocks_.emplace_back(b);
                }
            }

        } while (it->Next(level));
    }


    void extract_words_and_map_to_blocks() {
        words_.clear();

        std::unique_ptr<tesseract::ResultIterator> it(api_->GetIterator());
        if (!it) return;

        tesseract::PageIteratorLevel level = tesseract::RIL_WORD;

        do {
            auto word_text = make_text(it->GetUTF8Text(level));

            float conf = it->Confidence(level);

            int x1, y1, x2, y2;
            bool has_box = it->BoundingBox(level, &x1, &y1, &x2, &y2);

            // READ: This check is real need?
            if (!has_box) {
                if (word_text) word_text.get_deleter();
                continue;
            }

            Word w;
            w.text = word_text ? std::string(word_text.get()) : "";
            w.box = Rect(x1, y1, x2 + 5, y2 + 7); // simple shift for best rendering
            w.confidence = conf;
            w.block_id = find_block_for_word(w);

            words_.emplace_back(std::move(w));

            if (word_text) word_text.get_deleter();
        }
        while (it->Next(level));
    }

    int find_block_for_word(const Word& w){
        double cx = (w.box.x1 + w.box.x2) / 2.0;
        double cy = (w.box.y1 + w.box.y2) / 2.0;

        for (const auto& b : blocks_)
            if (cx >= b.box.x1 && cx <= b.box.x2 && cy >= b.box.y1 && cy <= b.box.y2) 
                return b.id;

        return -1;
    }

private:
    cv::Mat img_original_;  // BGR
    cv::Mat img_rgb_;       //RGB

    std::unique_ptr<tesseract::TessBaseAPI, void(*)(tesseract::TessBaseAPI*)> api_;

    std::vector<Block> blocks_;
    std::vector<Word> words_;
};

#endif //__TESSERACT_PARSER__
