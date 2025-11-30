#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <qapplication.h>
#include <qpushbutton.h>
#include <qwidget.h>
#include <QPainter>
#include <QString>
#include <QMessageBox>
#include <QLabel>

#include "Parser.h"


int main(int argc, char *argv[]){
    cv::Mat img = cv::imread("/home/alex/opencv_test/screenshot.png");
    Parser p(img);

    p.process();

    p.save_detected_words("all_words.txt");

    auto words = p.get_words(); 
    auto blocks = p.get_blocks();

    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle("Transparent Always-on-Top");
    window.setAttribute(Qt::WA_TranslucentBackground);
    // window.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    // window.setStyleSheet("background: rgba(0, 0, 0, 0);"); 
    
    // QVBoxLayout* layout = new QVBoxLayout(&window);

    //widget with translation text
    QWidget* Text = new QWidget(&window);
    Text->setWindowTitle("text");
    Text->setWindowFlags(Qt::ToolTip);

    QLabel* label = new QLabel(Text);
    label->setWordWrap(true);             // включаем перенос строк
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // label->setStyleSheet("color: white; background: rgba(0,0,0,180); padding: 4px;");

    QFrame* line = new QFrame(Text);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setStyleSheet("color: white; background-color: white;");

    QLabel* label2 = new QLabel(Text);
    label2->setWordWrap(true);             // включаем перенос строк
    label2->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    for(auto a : words){
        auto [x1, y1, x2, y2] = a.box;

        QPushButton* btn = new QPushButton("", &window);
        btn->setGeometry(x1, y1, x2 - x1, y2 - y1);
        btn->setStyleSheet("background-color: rgba(255, 255, 0, 0.44);");
        btn->setProperty("word_text", QString::fromStdString(a.text));
        // btn->setProperty("block_text", QString::fromStdString(std::to_string(a.block_id)));
        for(auto& it : blocks){
            if (a.block_id == it.id){
                btn->setProperty("block_text", QString::fromStdString(it.text));
                break;
            }
        }

        QObject::connect(btn, &QPushButton::clicked, [btn, a, Text, &label, &line, &label2](){
            QString word = btn->property("word_text").toString();
            QString block = btn->property("block_text").toString();
            Text->setGeometry(a.box.x1, a.box.y1 + 50, 200, 100);

            // std::cout <<  btn->property("block_text").toString().toStdString() << '\n';

            //word
            // label->setMaximumWidth(250);
            label->setWordWrap(true);
            label->setText(word);
            label->adjustSize();

            //block
            // label2->setMaximumWidth(250);
            label2->setWordWrap(true);
            label2->setText(block);
            label2->adjustSize();

            // ширина линии = ширина блока - небольшие отступы
            int lineWidth = Text->width();
            // координата по центру блока
            int lineY = Text->height() / 2;
            line->setGeometry(10, lineY, lineWidth, 2);

            label2->setGeometry(0, lineY + 10, lineWidth, Text->height() / 2);

            // Перемещаем и растягиваем блоk
            Text->setGeometry(a.box.x1, a.box.y1 + 50, lineWidth + 100, lineY * 2 + 100);
            Text->show();
        });
    }

    window.resize(1920, 1080);
    window.show();

    return app.exec();
}
