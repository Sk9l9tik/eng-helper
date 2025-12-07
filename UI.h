#ifndef __APPUI__
#define __APPUI__

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <opencv2/ml.hpp>
#include <qapplication.h>
#include <qpushbutton.h>
#include <qwidget.h>
#include <string>

#include "LLM.h"
#include "Parser.h"

#undef __MY_LOG__ // disable logs

void delete_escapes(std::string& str, const std::vector<std::string>& escapes);


std::pair<std::string,std::string> parse_response(const std::string &ans);


int draw_interface(QApplication& app, const std::vector<Word>& words, const std::vector<Block>& blocks);

#endif // __APPUI__
