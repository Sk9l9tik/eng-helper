#include "UI.h"


namespace {

const char* kPopupStyle = R"(
#popup { background-color: rgba(28, 28, 32, 245); border: 1px solid #3a3a42; border-radius: 8px; }
QLabel { color: #e6e6e6; background: transparent; font-size: 13px; }
#caption { color: #8a8a93; font-size: 11px; }
#captionLine { background-color: #44444c; }
#word { color: white; font-size: 20px; font-weight: 600; }
#ipa { color: #8a8a93; font-size: 12px; }
#pos { color: #1c1c20; background-color: #9aa0a6; border-radius: 3px; padding: 0px 4px; font-size: 11px; }
#translation { color: #b8b8c0; }
#anki { color: #e6e6e6; background-color: #34343c; border: 1px solid #4a4a54; border-radius: 4px; padding: 2px 8px; font-size: 11px; }
#anki:hover { background-color: #44444c; }
#anki:disabled { color: #8a8a93; }
)";

const char* kWordStyle =
    "QPushButton { background-color: transparent; border: 1px solid rgba(255, "
    "255, 0, 170); }"
    "QPushButton:hover { background-color: rgba(255, 255, 0, 50); }";
const char* kSelectedWordStyle =
    "QPushButton { background-color: rgba(229, 193, 0, 110); border: 1px solid "
    "rgb(229, 193, 0); }";

const int kMaxSenses = 3;

QString html(const std::string& s) {
  return QString::fromStdString(s).toHtmlEscaped();
}

// Section title with a line to the right: "Dictionary ———————"
QWidget* make_caption(const QString& text, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* label = new QLabel(text, row);
  label->setObjectName("caption");
  auto* line = new QFrame(row);
  line->setObjectName("captionLine");
  line->setFixedHeight(1);

  layout->addWidget(label);
  layout->addWidget(line, 1);
  return row;
}

// Translation card shown next to the clicked word
class Popup : public QFrame {
public:
  std::function<void()> on_add_to_anki;

  explicit Popup(QWidget* parent) : QFrame(parent) {
    setObjectName("popup");
    setStyleSheet(kPopupStyle);
    setFixedWidth(380);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(6);

    layout->addWidget(make_caption("Dictionary", this));

    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    word_ = new QLabel(this);
    word_->setObjectName("word");
    ipa_ = new QLabel(this);
    ipa_->setObjectName("ipa");
    pos_ = new QLabel(this);
    pos_->setObjectName("pos");
    head->addWidget(word_);
    head->addWidget(ipa_);
    head->addWidget(pos_);
    head->addStretch();
    anki_ = new QPushButton(this);
    anki_->setObjectName("anki");
    anki_->setCursor(Qt::PointingHandCursor);
    connect(anki_, &QPushButton::clicked, this, [this] {
      if (on_add_to_anki)
        on_add_to_anki();
    });
    head->addWidget(anki_);
    layout->addLayout(head);

    in_context_ = new QLabel(this);
    in_context_->setWordWrap(true);
    in_context_->setTextFormat(Qt::RichText);
    layout->addWidget(in_context_);

    senses_ = new QLabel(this);
    senses_->setWordWrap(true);
    senses_->setTextFormat(Qt::RichText);
    layout->addWidget(senses_);

    layout->addSpacing(4);
    layout->addWidget(make_caption("Context", this));

    context_ = new QLabel(this);
    context_->setWordWrap(true);
    context_->setTextFormat(Qt::RichText);
    layout->addWidget(context_);

    translation_ = new QLabel(this);
    translation_->setObjectName("translation");
    translation_->setWordWrap(true);
    layout->addWidget(translation_);

    hide();
  }

  void show_word(const QRect& anchor, const std::string& word,
                 const std::string& context,
                 const std::optional<DictEntry>& entry,
                 const QString& dict_error) {
    anchor_ = anchor;
    const std::string clean = Dictionary::strip_punct(word);
    ++shown_;
    word_text_ = clean;
    context_text_ = QString::fromStdString(context).simplified().toStdString();
    entry_ = entry;
    word_ru_.clear();
    sentence_ru_.clear();
    // the card is added with the translation in context, so wait for it
    set_anki_state(shown_, "Translating…", false);

    word_->setText(QString::fromStdString(entry ? entry->headword : clean));
    ipa_->setText(entry ? QString::fromStdString(entry->ipa) : "");
    ipa_->setVisible(entry && !entry->ipa.empty());

    const auto senses = pick_senses(entry);
    pos_->setText(senses.empty() ? ""
                                 : QString::fromStdString(senses.front().pos));
    pos_->setVisible(!senses.empty() && !senses.front().pos.empty());

    if (!dict_error.isEmpty())
      senses_->setText("<span style='color:#d08770'>" +
                       dict_error.toHtmlEscaped() + "</span>");
    else if (senses.empty())
      senses_->setText(
          "<span style='color:#8a8a93'>Not found in the dictionary</span>");
    else
      senses_->setText(senses_html(senses));

    context_->setText(highlight(context, clean));
    translated_.clear();
    sentence_.clear();
    set_in_context("…");
    translation_->setText("…");

    adjustSize();
    place();
    show();
    raise();
  }

  // The model answers with two lines: the word as used in the sentence, then
  // the whole sentence
  void append_translation(const std::string& piece) {
    translated_ += QString::fromStdString(piece);
    const QString text = translated_.trimmed();
    const int nl = text.indexOf('\n');
    const QString first = (nl < 0 ? text : text.left(nl)).trimmed();

    if (nl < 0 && first.count(' ') >= 3) {
      // too long for a word: the model skipped the first line and is
      // translating the sentence
      set_in_context("");
      translation_->setText(first);
      word_ru_.clear();
      sentence_ru_ = first.toStdString();
    } else {
      set_in_context(first);
      translation_->setText(nl < 0 ? "…" : text.mid(nl + 1).trimmed());
      word_ru_ = first.toStdString();
      sentence_ru_ = nl < 0 ? "" : text.mid(nl + 1).trimmed().toStdString();
    }
    adjustSize();
    place();
  }

  // Sentence translation from a separate request, when the model gave only the
  // word
  void append_sentence(const std::string& piece) {
    sentence_ += QString::fromStdString(piece);
    const QString text = sentence_.trimmed();
    translation_->setText(text.isEmpty() ? "…" : text);
    sentence_ru_ = text.toStdString();
    adjustSize();
    place();
  }

  void finish_translation() { set_anki_state(shown_, "+ Anki", true); }

  // `shown` is what shown() returned when the request started: a newer word
  // ignores stale results
  void set_anki_state(int shown, const QString& text, bool enabled,
                      const QString& tooltip = "") {
    if (shown != shown_)
      return;
    anki_->setText(text);
    anki_->setEnabled(enabled);
    anki_->setToolTip(tooltip);
  }

  int shown() const { return shown_; }
  AnkiNote note() const {
    return make_anki_note(word_text_, entry_, context_text_, word_ru_,
                          sentence_ru_);
  }
  std::string headword() const {
    return entry_ ? entry_->headword : word_text_;
  }

  void set_translation_error(const QString& error) {
    finish_translation();
    set_in_context("");
    translation_->setText("<span style='color:#d08770'>" +
                          error.toHtmlEscaped() + "</span>");
    adjustSize();
    place();
  }

protected:
  // clicks inside the card must not reach the overlay, which hides the card
  void mousePressEvent(QMouseEvent* e) override { e->accept(); }

private:
  void set_in_context(const QString& word_ru) {
    in_context_->setVisible(!word_ru.isEmpty());
    in_context_->setText(
        "<span style='color:#8a8a93; font-size:11px'>In this context:</span> "
        "<span style='color:#e5c100; font-size:15px; font-weight:600'>" +
        word_ru.toHtmlEscaped() + "</span>");
  }

  // senses that have Russian translations first; only the first few are shown
  static std::vector<Sense> pick_senses(const std::optional<DictEntry>& entry) {
    std::vector<Sense> out;
    if (!entry)
      return out;
    for (const auto& s : entry->senses)
      if (!s.translations.empty() && out.size() < kMaxSenses)
        out.push_back(s);
    for (const auto& s : entry->senses)
      if (s.translations.empty() && out.size() < kMaxSenses)
        out.push_back(s);
    return out;
  }

  static QString senses_html(const std::vector<Sense>& senses) {
    QString out;
    for (size_t i = 0; i < senses.size(); ++i) {
      const auto& s = senses[i];
      QStringList ru;
      for (const auto& t : s.translations)
        ru << QString::fromStdString(t);

      QString def = QString::fromStdString(s.definition);
      if (def.size() > 140)
        def = def.left(137) + "…";

      out += "<div style='margin-bottom:6px'>";
      if (!ru.isEmpty())
        out += "<span style='color:white; font-size:14px; font-weight:600'>" +
               ru.join(", ").toHtmlEscaped() + "</span>";
      if (i > 0 && s.pos != senses[i - 1].pos && !s.pos.empty())
        out += " <span style='color:#8a8a93; font-size:11px'>" + html(s.pos) +
               "</span>";
      if (!def.isEmpty())
        out += (ru.isEmpty() ? "" : "<br>") +
               QString("<span style='color:#b8b8c0'>") + def.toHtmlEscaped() +
               "</span>";
      out += "</div>";
    }
    return out;
  }

  // context with the clicked word highlighted like on screen
  static QString highlight(const std::string& context,
                           const std::string& word) {
    QString text = QString::fromStdString(context).simplified();
    QString w = QString::fromStdString(word);
    int at = w.isEmpty() ? -1 : text.indexOf(w);
    if (at < 0)
      return text.toHtmlEscaped();
    return text.left(at).toHtmlEscaped() +
           "<span style='background-color:#e5c100; color:#111'>" +
           w.toHtmlEscaped() + "</span>" +
           text.mid(at + w.size()).toHtmlEscaped();
  }

  // under the word, or above it if there is no room below
  void place() {
    QWidget* p = parentWidget();
    int x = std::clamp(anchor_.left(), 0, std::max(0, p->width() - width()));
    int y = anchor_.bottom() + 6;
    if (y + height() > p->height())
      y = std::max(0, anchor_.top() - height() - 6);
    move(x, y);
  }

  QRect anchor_;
  QString translated_, sentence_;
  QLabel *word_, *ipa_, *pos_, *in_context_, *senses_, *context_, *translation_;
  QPushButton* anki_;

  // what the card is made from
  int shown_ = 0;
  std::string word_text_, context_text_, word_ru_, sentence_ru_;
  std::optional<DictEntry> entry_;
};

// Fullscreen transparent overlay: Esc closes the app, click on empty space
// hides the popup
class Overlay : public QWidget {
public:
  std::function<void()> on_background_click;

protected:
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Escape)
      QApplication::quit();
  }
  void mousePressEvent(QMouseEvent*) override {
    if (on_background_click)
      on_background_click();
  }
};

void make_layer_overlay(QWidget& window) {
  window.winId(); // create the QWindow before configuring layer-shell
  auto* layer = LayerShellQt::Window::get(window.windowHandle());
  layer->setLayer(LayerShellQt::Window::LayerOverlay);
  layer->setAnchors(LayerShellQt::Window::Anchors(
      LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom |
      LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
  layer->setExclusiveZone(-1);
  layer->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityExclusive);
  layer->setScope("lookupper");
}

// "неподвижный; ещё, всё ещё" without stress marks, for the translation prompt
std::string translation_options(const std::optional<DictEntry>& entry) {
  QStringList senses;
  if (entry)
    for (const auto& s : entry->senses) {
      QStringList ru;
      for (const auto& t : s.translations)
        ru << QString::fromStdString(t).remove(QChar(0x0301));
      if (!ru.isEmpty() && senses.size() < 8)
        senses << ru.join(", ");
    }
  return senses.join("; ").toStdString();
}

// filled by tools/fetch_dictionaries.py
std::string data_dir() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  std::string base =
      xdg && *xdg ? xdg : std::string(std::getenv("HOME")) + "/.local/share";
  return base + "/lookupper";
}

// Part of the screen around the word, like a screenshot in a mining card: the
// lines of its paragraph and sentence (at most a few above and below), with the
// word underlined
std::string picture_base64(const cv::Mat& screen,
                           const std::vector<Word>& words, size_t i) {
  const Word& w = words[i];
  const int h = std::max(1, w.line.y2 - w.line.y1);
  const int max_above = 4 * h, max_below = 4 * h;

  int x1 = w.line.x1, y1 = w.line.y1, x2 = w.line.x2, y2 = w.line.y2;
  for (const auto& o : words) {
    // the paragraph, and the sentence if it goes on in another one (subtitle
    // lines)
    if ((o.para_id != w.para_id && o.context != w.context) ||
        o.line.y1 < w.line.y1 - max_above || o.line.y2 > w.line.y2 + max_below)
      continue;
    x1 = std::min(x1, o.line.x1);
    y1 = std::min(y1, o.line.y1);
    x2 = std::max(x2, o.line.x2);
    y2 = std::max(y2, o.line.y2);
  }
  const int pad = h;
  cv::Rect roi =
      cv::Rect(cv::Point(x1 - pad, y1 - pad), cv::Point(x2 + pad, y2 + pad)) &
      cv::Rect(0, 0, screen.cols, screen.rows);
  if (roi.empty())
    return "";

  cv::Mat pic = screen(roi).clone();
  const int thickness = std::max(2, h / 8);
  const int y = w.box.y2 + thickness - roi.y;
  cv::line(pic, {w.box.x1 - roi.x, y}, {w.box.x2 - roi.x, y},
           cv::Scalar(0, 215, 255), thickness);

  constexpr int max_width = 800;
  if (pic.cols > max_width)
    cv::resize(pic, pic, cv::Size(), double(max_width) / pic.cols,
               double(max_width) / pic.cols, cv::INTER_AREA);

  std::vector<uchar> jpg;
  cv::imencode(".jpg", pic, jpg, {cv::IMWRITE_JPEG_QUALITY, 90});
  return QByteArray(reinterpret_cast<const char*>(jpg.data()), jpg.size())
      .toBase64()
      .toStdString();
}

} // namespace

int draw_interface(QApplication& app, const cv::Mat& screenshot,
                   const std::vector<Word>& words,
                   const std::vector<Block>& blocks) {

  static OllamaClient client;
  std::thread([] {
    try {
      client.warmup();
    } catch (...) {
    }
  }).detach();

  static Dictionary dictionary;
  const std::string dict_path = data_dir() + "/eng-rus/eng-rus";
  const QString dict_error =
      dictionary.load(dict_path)
          ? QString()
          : QString::fromStdString("Dictionary not found: " + dict_path +
                                   ".ifo");

  Overlay window;
  window.setWindowTitle("LookUpper");
  window.setAttribute(Qt::WA_TranslucentBackground);
  make_layer_overlay(window);

  Popup* popup = new Popup(&window);

  // coordinates from tesseract are in physical pixels, widgets use logical ones
  const double dpr = window.devicePixelRatioF();

  // only the answer to the latest click is shown
  auto request_id = std::make_shared<std::atomic<int>>(0);
  auto selected = std::make_shared<QPointer<QPushButton>>();
  auto selected_word = std::make_shared<size_t>(0);

  static Pronunciations pronunciations(data_dir() + "/audio");
  static AnkiClient anki;

  popup->on_add_to_anki = [popup, selected_word, &screenshot, &words] {
    const int shown = popup->shown();
    popup->set_anki_state(shown, "Adding…", false);

    AnkiNote note = popup->note();
    note.picture_base64 = picture_base64(screenshot, words, *selected_word);

    QPointer<Popup> target = popup;
    std::thread([note = std::move(note), word = popup->headword(), shown,
                 target]() mutable {
      QString state = "Added ✓", error;
      bool retry = false;
      try {
        note.audio_path = pronunciations.fetch(
            QString::fromStdString(word).toLower().toStdString());
        anki.add(note);
      } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
        retry = !error.contains("duplicate");
        state = retry ? "Failed: " +
                            (error.size() > 40 ? error.left(39) + "…" : error)
                      : "Already in Anki";
      }
      QMetaObject::invokeMethod(
          qApp,
          [=] {
            if (target)
              target->set_anki_state(shown, state, retry, error);
          },
          Qt::QueuedConnection);
    }).detach();
  };

  window.on_background_click = [popup, selected, request_id] {
    popup->hide();
    ++*request_id; // cancels the translation in progress
    if (*selected)
      (*selected)->setStyleSheet(kWordStyle);
  };

  for (size_t i = 0; i < words.size(); ++i) {
    const Word& a = words[i];
    auto [x1, y1, x2, y2] = a.box;

    QPushButton* btn = new QPushButton("", &window);
    btn->setGeometry(x1 / dpr - 3, y1 / dpr - 3, (x2 - x1) / dpr + 7,
                     (y2 - y1) / dpr + 7);
    btn->setStyleSheet(kWordStyle);

    const std::string& context = a.context;

    QObject::connect(
        btn, &QPushButton::clicked,
        [btn, i, word = a.text, context, popup, request_id, selected,
         selected_word, &dict_error]() {
#ifdef __MY_LOG__
          std::cout << "------>" << word << " | " << context << "\n\n";
#endif

          if (*selected)
            (*selected)->setStyleSheet(kWordStyle);
          *selected = btn;
          *selected_word = i;
          btn->setStyleSheet(kSelectedWordStyle);

          auto entry =
              dict_error.isEmpty() ? dictionary.lookup(word) : std::nullopt;
          popup->show_word(btn->geometry(), word, context, entry, dict_error);

          const int id = ++*request_id;
          QPointer<Popup> target = popup;
          auto deliver = [id, request_id, target](auto update) {
            QMetaObject::invokeMethod(
                qApp,
                [=]() {
                  if (target && id == *request_id)
                    update(target.data());
                },
                Qt::QueuedConnection);
          };

          std::thread([word, context, options = translation_options(entry), id,
                       request_id, deliver]() {
            std::string answer;
            try {
              // a newer click cancels this request, so ollama doesn't queue
              // stale answers
              client.translate(
                  Dictionary::strip_punct(word),
                  QString::fromStdString(context).simplified().toStdString(),
                  options,
                  [&](const std::string& piece) {
                    answer += piece;
                    deliver(
                        [piece](Popup* p) { p->append_translation(piece); });
                  },
                  [id, request_id] { return id != *request_id; });

              // small models sometimes stop after the word line: ask for the
              // sentence separately
              const QString text = QString::fromStdString(answer).trimmed();
              const int nl = text.indexOf('\n');
              const bool has_sentence =
                  nl < 0 ? text.count(' ') >= 3
                         : !text.mid(nl + 1).trimmed().isEmpty();
              if (!has_sentence)
                client.translate_sentence(
                    QString::fromStdString(context).simplified().toStdString(),
                    [&](const std::string& piece) {
                      deliver([piece](Popup* p) { p->append_sentence(piece); });
                    },
                    [id, request_id] { return id != *request_id; });
              deliver([](Popup* p) { p->finish_translation(); });
            } catch (const RequestCancelled&) {
            } catch (const std::exception& e) {
              deliver([msg = QString::fromStdString(e.what())](Popup* p) {
                p->set_translation_error(msg);
              });
            }
          }).detach();
        });
  }

  window.show();

  return app.exec();
}
