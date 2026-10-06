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
#include <optional>
#include <string>
#include <thread>

#include <LayerShellQt/Window>

#include <opencv2/core.hpp>

#include "Anki.h"
#include "Audio.h"
#include "Dictionary.h"
#include "LLM.h"
#include "Yandex.h"
#include "Parser.h"

#undef __MY_LOG__ // disable logs

// `parser` has recognized `area` of the screenshot; it is used again to
// recognize the area better on request. `cursor` is where the mouse was.
int draw_interface(QApplication& app, const cv::Mat& screenshot,
                   Parser& parser, cv::Rect area,
                   std::optional<cv::Point> cursor);

#endif // __APPUI__
