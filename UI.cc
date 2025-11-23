#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "Parser.h"

int main(int argc, char *argv[])
{

    cv::Mat img = cv::imread("/home/alex/opencv_test/screenshot.png");
    Parser p(img);

    p.process();

    p.save_detected_words("all_words.txt");

    auto words = p.get_words(); 

    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle("Transparent Always-on-Top");

    window.setAttribute(Qt::WA_TranslucentBackground);
    // window.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    // window.setStyleSheet("background: rgba(0, 0, 0, 0);"); 

    // QVBoxLayout* layout = new QVBoxLayout(&window);

    for(auto a : words){
        auto [x1, y1, x2, y2] = a.box;
        QPushButton* btn = new QPushButton(a.text.c_str(), &window);
        btn->setGeometry(x1, y1, x2 - x1, y2 - y1); // set button's left corner and right corner
        // btn->setStyleSheet("background-color: yellow;"); 
        QObject::connect(btn, &QPushButton::clicked, &app, &QApplication::quit);
    }
    // QPushButton* btn = new QPushButton("Close", &window);
    // btn->setGeometry(50, 60, 200, 120); // set button's left corner and right corner
    // // btn->setStyleSheet("background-color: rgba(255, 255, 255, 200);"); 
    // QObject::connect(btn, &QPushButton::clicked, &app, &QApplication::quit);
    //

    // layout->addWidget(btn);
    // window.setLayout(layout);

    window.resize(1920, 1080);
    window.show();

    return app.exec();
}
