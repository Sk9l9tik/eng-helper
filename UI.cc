#include "UI.h"

void delete_escapes(std::string& str, const std::vector<std::string>& escapes){
    // std::array<std::string, 4> escapes = {"\\u003c", "\\u003e", "\\n", "\\\"" };
    for(const auto& st : escapes){
        size_t pos{};
        while ((pos = str.find(st, pos)) != std::string::npos) {
            str.erase(pos, st.size());
        }
    }
}

std::pair<std::string,std::string> parse_response(
    // std::unordered_map<std::string, std::pair<std::string, std::string>> &map,
    // const std::vector<std::string> &word, 
    const std::string &ans) {
    std::string strs = ans;

    delete_escapes(strs, {"\\u003e", "\\u003c"});


    auto it = strs.find("\"text\":");
    auto e = strs.find("\"index\"");

    std::string str;
    if (it != std::string::npos && e != std::string::npos) {
        str = strs.substr(it + 8, e - it - 9);
    }

#ifdef __MY_LOG__
    std::cout << "------ AI anser:";
    std::cout << ans;

    std::cout << "------ finally string:";
    std::cout << str << "\n\n";
#endif

    delete_escapes(str, {"\\n", "\\\""});


    auto colon_pos = str.find("#");

    return {str.substr(0, colon_pos), str.substr(colon_pos+1, str.size())};
}

// TODO: Move inside call OllamaClient
int draw_interface(QApplication& app, const std::vector<Word>& words, const std::vector<Block>& blocks) {

    OllamaClient client;

    QWidget window;
    window.setWindowTitle("Transparent Always-on-Top");
    window.setAttribute(Qt::WA_TranslucentBackground);
    // window.setWindowFlags(Qt::FramelessWindowHint |
    // Qt::WindowStaysOnTopHint); window.setStyleSheet("background: rgba(0, 0,
    // 0, 0);");

    // QVBoxLayout* layout = new QVBoxLayout(&window);

    // widget with translation text
    QWidget *Text = new QWidget(&window);
    Text->setWindowTitle("text");
    Text->setWindowFlags(Qt::ToolTip);

    QLabel *label = new QLabel(Text);
    label->setWordWrap(true); // включаем перенос строк
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // label->setStyleSheet("color: white; background: rgba(0,0,0,180);
    // padding: 4px;");

    QFrame *line = new QFrame(Text);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setStyleSheet("color: white; background-color: white;");

    QLabel *label2 = new QLabel(Text);
    label2->setWordWrap(true); // включаем перенос строк
    label2->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    for (auto a : words) {
        auto [x1, y1, x2, y2] = a.box;

        QPushButton *btn = new QPushButton("", &window);
        btn->setGeometry(x1-3, y1-3, x2 - x1+7, y2 - y1+7);
        btn->setStyleSheet("background-color: rgba(255, 255, 0, 0.44);");
        btn->setProperty("word_text", QString::fromStdString(a.text));

        for (auto &it : blocks) {
            if (a.block_id == it.id) {
                btn->setProperty("block_text", QString::fromStdString(it.text));
                break;
            }
        }

        // QString word = btn->property("word_text").toString();
        // QString block = btn->property("block_text").toString();
        // std::string prompt = word.toStdString() + ":" + block.toStdString();
        //
        // std::string sblock = client.sendRequest(prompt);

        QObject::connect(
            btn, &QPushButton::clicked,
            [btn, a, Text, &label, 
            &line, &label2, &client /*, &word, &block, &sblock*/]() {
                QString word = btn->property("word_text").toString();
                QString block = btn->property("block_text").toString();

                std::string prompt = "\"" + word.toStdString() + "$" + block.toStdString() + "\"";
                // std::string prompt = word.toStdString() + ":" + block.toStdString();
                
                #ifdef __MY_LOG__
                std::cout << "------>" << word.toStdString() << "\n";
                std::cout << "------>" << block.toStdString() << "\n";
                std::cout << "------>" << prompt << "\n\n";
                #endif

               std::pair<std::string, std::string> sblock = parse_response(client.sendRequest(prompt));

                // QString word = btn->property("word_text").toString();
                // QString block = btn->property("block_text").toString();
                // std::string prompt = word.toStdString() + ":" +
                // block.toStdString();
                //
                // std::string sblock = client.sendRequest(prompt);

                // std::string st = "";
                // std::vector<std::string> sv;
                //
                // std::stringstream ss(sblock);
                // int cnt{15};
                // std::string str;
                // int j = 0;
                // while(ss >> str){
                //     sv.emplace_back(str);
                //     if (str == word.toStdString()){
                //         for(int i = 1; i < cnt; ++i){
                //             st += sv[j - i];
                //         }
                //         --cnt;
                //     }
                //     if (cnt != 0 && cnt != 15){
                //        st += str;
                //        --cnt;
                //     }
                //     ++j;
                // }

                Text->setGeometry(a.box.x1, a.box.y1 + 50, 200, 100);

                // std::cout <<
                // btn->property("block_text").toString().toStdString() << '\n';


                // ширина линии = ширина блока - небольшие отступы
                int lineWidth = Text->width();
                // координата по центру блока
                int lineY = Text->height() / 2;
                line->setGeometry(10, lineY, lineWidth +100, 2);

                label2->setGeometry(0, lineY + 10, lineWidth, Text->height() / 2);

                // Перемещаем и растягиваем блоk
                Text->setGeometry(a.box.x1, a.box.y1 + 50, lineWidth + 100,
                                lineY * 2 + 100);


                // word
                //  label->setMaximumWidth(250);
                label->setWordWrap(true);
                // label->setText(word);
                label->setText(QString::fromStdString(sblock.first));
                label->adjustSize();

                // block
                //  label2->setMaximumWidth(250);
                label2->setWordWrap(true);
                // label2->setText(block);
                label2->setText(QString::fromStdString(sblock.second));
                label2->adjustSize();



                
                Text->show();
            });
    }

    window.resize(1920, 1080);
    window.show();

    return app.exec();
}
