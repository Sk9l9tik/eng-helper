#ifndef __APPUI__
#define __APPUI__

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <memory>
#include <qapplication.h>
#include <qpushbutton.h>
#include <qwidget.h>
#include <string>
#include <thread>

#include <LayerShellQt/Window>

#include <opencv2/core.hpp>

#include "Anki.h"
#include "Audio.h"
#include "Dictionary.h"
#include "LLM.h"
#include "Parser.h"

#undef __MY_LOG__ // disable logs

int draw_interface(QApplication& app, const cv::Mat& screenshot,
                   const std::vector<Word>& words,
                   const std::vector<Block>& blocks);

#endif // __APPUI__
