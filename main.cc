#include "UI.h"

void make_screenshot(){
    [[deprecated]] const char old_screenshot_command[] = {"grim - | convert - -shave 1x1 PNG:- | wl-copy && wl-paste > screenshot.png"};

    const char screenshot_command[] = {"grim - | magick - -shave 1x1 PNG:- | wl-copy && wl-paste > /home/alex/opencv_test/screenshot.png"};

    std::system(screenshot_command);
}

int main(int argc, char** argv){

    make_screenshot();

    QApplication app(argc, argv);

    std::string image = "/home/alex/opencv_test/screenshot.png"; 

    cv::Mat img = cv::imread(image);
    Parser p(img);

    p.process();

    p.save_detected_words("all_words.txt");


    auto words = p.get_words();
    auto blocks = p.get_blocks();

    draw_interface(app, words, blocks);

    return 0;
}

