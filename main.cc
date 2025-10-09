#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <opencv2/opencv.hpp>

#include <iostream>
#include <cstdlib>
#include <tesseract/publictypes.h>
#include <utility>
#include <vector>
#include <fstream>
#include <set>

cv::Mat make_screenshot() {
    std::string output("screenshot.png");
    std::string command("grim - | convert - -shave 1x1 PNG:- | wl-copy && wl-paste >" + output );

    int ret = std::system (command.c_str());

    if (ret != 0) {
        std::cerr << "Error taking screenshot" << '\n';
        return cv::Mat();
    }

    cv::Mat img = cv::imread(output);

    if(img.empty()) {
        std::cerr << "Error loading image" <<  '\n';
        return cv::Mat();
    }

    return img;
}

int main() {
    std::vector<std::string> words;
    std::set<std::string> blocks;
    std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> coordinates;

    cv::Mat img = make_screenshot();

    if(img.empty()){
        return 1;
    }

    tesseract::TessBaseAPI * ocr = new tesseract::TessBaseAPI();
    if (ocr->Init(NULL, "eng")){
        std::cerr << "Couldn't initialize tesseract." << '\n';
        return 1;
    }
    ocr->SetPageSegMode(tesseract::PSM_AUTO);

    cv::Mat img_rgb;
    cv::cvtColor(img, img_rgb, cv::COLOR_BGR2RGB);

    ocr->SetImage(img_rgb.data, img_rgb.cols, img_rgb.rows, 3, img_rgb.step);

    ocr->Recognize(0);
    tesseract::ResultIterator* ri = ocr->GetIterator();
    tesseract::PageIteratorLevel level = tesseract::RIL_WORD;

    tesseract::ResultIterator* pi = ocr->GetIterator();


    if (ri != nullptr) {
        do {
            const char* word = ri->GetUTF8Text(level);
            float conf = ri->Confidence(level);

            //word coordinates
            int x1, y1, x2, y2;
            ri->BoundingBox(level, &x1, &y1, &x2, &y2);

            //block coordinates
            int bx1, by1, bx2, by2;
            pi->BoundingBox(tesseract::RIL_BLOCK, &bx1, &by1, &bx2, &by2);


            bool word_in_block = (x1 >= bx1) && (y1 >= by1) && (x2 <= bx2) && (y2 <= by2);


            if(!word_in_block){
                blocks.emplace(ri->GetUTF8Text(tesseract::RIL_BLOCK));
                // std::cout << ri->GetUTF8Text(tesseract::RIL_BLOCK) << '\n';
            }
            else if(!pi){
                pi->Next(tesseract::RIL_BLOCK);
            }



            // Draw rectangle
            cv::rectangle(img, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 255, 255), 1);
            //cv::rectangle(img, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 255, 255, 0.5), cv::FILLED);
            
            coordinates.emplace_back(std::make_pair(x1, y1), std::make_pair(x2, y2));

            // Optionally show the word
            if (word) {
                words.emplace_back(word);

                cv::putText(img, word, cv::Point(x1, y1 - 5),
                            cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 255, 0), 1);
                delete[] word;
            }

        } while (ri->Next(level));
    }

    std::ofstream all_words("all_words.txt");

    std::for_each(blocks.begin(), blocks.end(), [&all_words](const std::string& block){
            all_words << block << '\n';
    });

    std::for_each(coordinates.begin(), coordinates.end(), [&all_words](const auto& coordinate){
            auto [p1, p2] = coordinate;
            auto [x1, y1] = p1;
            auto [x2, y2] = p2;

            all_words << x1 << ' ' << y1 << '\n';
            all_words << x2 << ' ' << y2 << "\n\n";
    });

    cv::imwrite("screenshot_highlighted.png", img);

    ocr->End();

    cv::imshow("screenshot", img);

    cv::waitKey(0);

    return 0;
}
