#include "UI.h"

#include <QRegularExpression>

#include <numeric>


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
  std::function<void()> on_retranslate;
  std::function<void()> on_improve_ocr;

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

    // the recognized sentence with a button to recognize the area again, right
    // above the one to translate the sentence again
    auto* context_row = new QHBoxLayout();
    context_row->setSpacing(8);
    context_ = new QLabel(this);
    context_->setWordWrap(true);
    context_->setTextFormat(Qt::RichText);
    context_row->addWidget(context_, 1);
    improve_ = new QPushButton(this);
    improve_->setObjectName("anki");
    improve_->setCursor(Qt::PointingHandCursor);
    connect(improve_, &QPushButton::clicked, this, [this] {
      if (on_improve_ocr)
        on_improve_ocr();
    });
    context_row->addWidget(improve_, 0, Qt::AlignTop);
    layout->addLayout(context_row);

    // the sentence translation with a button to translate it again
    auto* translation_row = new QHBoxLayout();
    translation_row->setSpacing(8);
    translation_ = new QLabel(this);
    translation_->setObjectName("translation");
    translation_->setWordWrap(true);
    translation_row->addWidget(translation_, 1);
    retranslate_ = new QPushButton("↻", this);
    retranslate_->setObjectName("anki");
    retranslate_->setCursor(Qt::PointingHandCursor);
    retranslate_->setToolTip("Translate the sentence again");
    connect(retranslate_, &QPushButton::clicked, this, [this] {
      if (on_retranslate)
        on_retranslate();
    });
    translation_row->addWidget(retranslate_, 0, Qt::AlignTop);
    layout->addLayout(translation_row);

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
    options_ = dict_options(entry);
    word_ru_.clear();
    sentence_ru_.clear();
    sentence_only_ = false;
    set_improve_state("OCR", true, "Recognize the area again");
    // the card is added with the translation in context, so wait for it
    set_anki_state(shown_, "Translating…", false);
    retranslate_->setEnabled(false);

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

  // The local model streams the sentence translation as text with the word in
  // **…** (see marked_md)
  void append_translation(const std::string& piece) {
    translated_ += QString::fromStdString(piece);
    show_marked(bold_md_to_html(translated_, false), false);
  }

  void show_marked_final() {
    show_marked(bold_md_to_html(translated_, true), true);
  }

  // Translation of the word alone, for when the sentence translation does
  // not show which words are its: ignored if a dictionary translation was
  // found there
  void set_word_translation(const QString& word) {
    if (!word_ru_.empty() || word.isEmpty())
      return;
    show_result(sentence_shown_, find_any(sentence_shown_, {word}), word, true);
  }

  // Another translation of the sentence ("↻"): the word is already known,
  // only marked in the new sentence
  void append_sentence(const std::string& piece) {
    sentence_ += QString::fromStdString(piece);
    auto [sentence, span] = parse_marked(bold_md_to_html(sentence_, false), false);
    if (!span.len)
      span = find_any(sentence, options_);
    if (!span.len)
      span = find_any(sentence, {fixed_word_});
    show_result(sentence, span, fixed_word_, true);
  }

  // a new translation of the same word is coming
  // Another translation of the sentence is coming; the word in context stays
  void restart_sentence() {
    sentence_only_ = true;
    fixed_word_ = QString::fromStdString(word_ru_);
    sentence_.clear();
    translation_->setText("…");
    set_anki_state(shown_, "Translating…", false);
    retranslate_->setEnabled(false);
  }

  // a translation of the word is found in the sentence
  bool word_found() const { return !word_ru_.empty(); }

  void finish_translation() {
    retranslate_->setEnabled(true);
    if (word_ru_.empty())
      set_in_context("");
    set_anki_state(shown_, "+ Anki", true);
  }

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

  void set_improve_state(const QString& text, bool enabled,
                         const QString& tooltip) {
    improve_->setText(text);
    improve_->setEnabled(enabled);
    improve_->setToolTip(tooltip);
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
    if (!sentence_only_)
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

  struct Span {
    int at = 0, len = 0;
  };

public:
  // Translation of the sentence with the clicked word in <b>: the tag is
  // carried over to the word's translation by Yandex, and by the local model
  // as **…** (bold_md_to_html). Without it the word is found by its
  // dictionary translations.
  void show_marked(const QString& html, bool done = true) {
    auto [sentence, span] = parse_marked(html, done);
    const QString marked = sentence.mid(span.at, span.len);

    // the dictionary form if the marked word is one of the dictionary words
    QString word = marked;
    for (const auto& option : options_)
      if (find_phrase(word, option, true).len) {
        word = option;
        break;
      }
    if (!span.len) {
      word.clear();
      span = find_any(sentence, options_, &word);
      // "цветок" found as "цветущих": the word as it is in the sentence,
      // not a dictionary word of another part of speech
      const QString hit = sentence.mid(span.at, span.len);
      if (span.len && !find_phrase(hit, word, true).len)
        word = hit;
    }
    show_result(sentence, span, word, done);
  }

  // The local model marks the word with **…** (markdown) much more often than
  // with <b>: text -> escaped HTML with <b>. While streaming, a lone "*" at the
  // end may be the first half of "**" and is not shown.
  static QString bold_md_to_html(const QString& text, bool done) {
    QString t = text;
    if (!done && t.endsWith('*') && !t.endsWith("**"))
      t.chop(1);
    const QStringList parts = t.toHtmlEscaped().split("**");
    QString out = parts.front();
    for (int i = 1; i < parts.size(); ++i)
      out += (i % 2 ? "<b>" : "</b>") + parts[i];
    return out;
  }

private:
  // Sentence without tags and the text of its first <b>.
  // `done`: no more text is coming; while streaming, an unclosed <b> marks
  // up to the end, and a tag still being written is not shown.
  static std::pair<QString, Span> parse_marked(QString html, bool done) {
    if (!done)
      if (const int lt = html.lastIndexOf('<'); lt > html.lastIndexOf('>'))
        html.truncate(lt);
    static const QRegularExpression tag("<(/?)([a-zA-Z]*)[^>]*>");
    QString sentence;
    Span span;
    bool open = false; // inside the first <b>
    int from = 0;
    auto text = [](QString t) {
      return t.replace("&lt;", "<")
          .replace("&gt;", ">")
          .replace("&quot;", "\"")
          .replace("&#39;", "'")
          .replace("&nbsp;", " ")
          .replace("&amp;", "&");
    };
    for (auto it = tag.globalMatch(html); it.hasNext();) {
      const auto m = it.next();
      sentence += text(html.mid(from, m.capturedStart() - from));
      from = m.capturedEnd();
      if (m.captured(2).toLower() != "b")
        continue;
      if (m.captured(1).isEmpty() && !span.len && !open) {
        span.at = sentence.size();
        open = true;
      } else if (!m.captured(1).isEmpty() && open) {
        span.len = sentence.size() - span.at;
        open = false;
      }
    }
    sentence += text(html.mid(from));
    if (open) // still streaming the word
      span.len = sentence.size() - span.at;
    // whitespace at the tag edges ("<b> передам </b>") and around the sentence
    span.at = std::min<int>(span.at, sentence.size());
    span.len = std::min<int>(span.len, sentence.size() - span.at);
    while (span.len && sentence[span.at].isSpace()) {
      ++span.at;
      --span.len;
    }
    while (span.len && sentence[span.at + span.len - 1].isSpace())
      --span.len;
    int lead = 0;
    while (lead < sentence.size() && sentence[lead].isSpace())
      ++lead;
    sentence = sentence.mid(lead);
    while (!sentence.isEmpty() && sentence.back().isSpace())
      sentence.chop(1);
    span.at = std::max(0, span.at - lead);
    return {sentence, span};
  }

  void show_result(const QString& sentence, Span span, const QString& word,
                   bool done) {
    sentence_shown_ = sentence;
    set_in_context(word.isEmpty() && !done ? "…" : word);
    const QString before = sentence.left(span.at),
                  marked = sentence.mid(span.at, span.len),
                  after = sentence.mid(span.at + span.len);
    translation_->setText(
        sentence.isEmpty()
            ? "…"
            : before.toHtmlEscaped() +
                  (span.len ? "<i style='color:#e5c100'>" +
                                  marked.toHtmlEscaped() + "</i>"
                            : "") +
                  after.toHtmlEscaped());
    word_ru_ = word.toStdString();
    // the Anki card shows "[...]" in bold
    sentence_ru_ = (span.len ? before + "[" + marked + "]" + after : sentence)
                       .toStdString();
    adjustSize();
    place();
  }

  // Same word in another form: "сообщать" ~ "сообщу", "искусство" ~
  // "искусства". Short words must match exactly
  // `strict`: only the ending differs, "берегу" ~ "берег" but not "цветущих"
  // ~ "цветок" (another part of speech with the same root)
  static bool same_stem(QString a, QString b, bool strict = false) {
    a = a.toLower().replace(QChar(u'ё'), QChar(u'е'));
    b = b.toLower().replace(QChar(u'ё'), QChar(u'е'));
    const int n = std::min(a.size(), b.size());
    if (n < 4)
      return a == b;
    int common = 0;
    while (common < n && a[common] == b[common])
      ++common;
    if (strict)
      return common >= n - 1;
    return common >= 4 && common * 10 >= n * 6;
  }

  // `phrase` ("всё ещё") in `sentence`, word by word
  static Span find_phrase(const QString& sentence, const QString& phrase,
                          bool strict = false) {
    static const QRegularExpression word_re("[\\p{L}-]+");
    QStringList want;
    for (auto it = word_re.globalMatch(phrase); it.hasNext();)
      want << it.next().captured();
    if (want.isEmpty())
      return {};

    std::vector<QRegularExpressionMatch> words;
    for (auto it = word_re.globalMatch(sentence); it.hasNext();)
      words.push_back(it.next());
    for (size_t i = 0; i + want.size() <= words.size(); ++i) {
      int k = 0;
      while (k < want.size() &&
             same_stem(words[i + k].captured(), want[k], strict))
        ++k;
      if (k == want.size()) {
        const auto& last = words[i + k - 1];
        return {int(words[i].capturedStart()),
                int(last.capturedEnd() - words[i].capturedStart())};
      }
    }
    return {};
  }

  // The first of `phrases` found in `sentence`, then, if none is there whole,
  // a long word of one: "нотариально заверенный" -> "с нотариальным
  // удостоверением". `found` gets the phrase.
  static Span find_any(const QString& sentence, const QStringList& phrases,
                       QString* found = nullptr) {
    for (const auto& phrase : phrases)
      if (Span span = find_phrase(sentence, phrase); span.len) {
        if (found)
          *found = phrase;
        return span;
      }
    static const QRegularExpression word_re("\\p{L}{5,}");
    for (const auto& phrase : phrases) {
      QStringList long_words;
      for (auto it = word_re.globalMatch(phrase); it.hasNext();)
        long_words << it.next().captured();
      if (long_words.size() < 2)
        continue; // a single word was tried whole
      std::sort(long_words.begin(), long_words.end(),
                [](const QString& a, const QString& b) {
                  return a.size() > b.size();
                });
      for (const auto& w : long_words)
        if (Span span = find_phrase(sentence, w); span.len) {
          if (found)
            *found = phrase;
          return span;
        }
    }
    return {};
  }

  // "банк", "берег", "всё ещё" without stress marks
  static QStringList dict_options(const std::optional<DictEntry>& entry) {
    QStringList out;
    if (entry)
      for (const auto& s : entry->senses)
        for (const auto& t : s.translations)
          for (auto part : QString::fromStdString(t)
                               .remove(QChar(0x0301))
                               .split(QRegularExpression("[,;]"))) {
            part = part.trimmed();
            if (!part.isEmpty() && !out.contains(part))
              out << part;
          }
    return out;
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
  QString sentence_shown_; // the translation in the card, without markup
  // dictionary translations, to find the word in the sentence translation
  QStringList options_;
  QLabel *word_, *ipa_, *pos_, *in_context_, *senses_, *context_, *translation_;
  QPushButton *anki_, *retranslate_, *improve_;
  // "↻": only the sentence is translated again, the word in context stays
  bool sentence_only_ = false;
  QString fixed_word_;

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

// filled by tools/fetch_dictionaries.py
std::string data_dir() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  std::string base =
      xdg && *xdg ? xdg : std::string(std::getenv("HOME")) + "/.local/share";
  return base + "/lookupper";
}

// Sentence as HTML with the word in <b>: Yandex carries the tag over to the
// word's translation. Case-insensitive: the sentence may be re-cased.
std::string marked_html(const std::string& sentence, const std::string& word) {
  const QString s = QString::fromStdString(sentence),
                w = QString::fromStdString(word);
  const int at = w.isEmpty() ? -1 : s.indexOf(w, 0, Qt::CaseInsensitive);
  if (at < 0)
    return s.toHtmlEscaped().toStdString();
  return (s.left(at).toHtmlEscaped() + "<b>" +
          s.mid(at, w.size()).toHtmlEscaped() + "</b>" +
          s.mid(at + w.size()).toHtmlEscaped())
      .toStdString();
}

// Sentence with the word in **…** for the local model: unlike <b>, which it
// keeps rarely and which spoils the translation ("make <b>sacrifices</b> to it"
// -> "будем готовы к этому"), it carries the markdown over to the word's
// translation, so one request gives both the sentence and the word
std::string marked_md(const std::string& sentence, const std::string& word) {
  const QString s = QString::fromStdString(sentence),
                w = QString::fromStdString(word);
  const int at = w.isEmpty() ? -1 : s.indexOf(w, 0, Qt::CaseInsensitive);
  if (at < 0)
    return sentence;
  return (s.left(at) + "**" + s.mid(at, w.size()) + "**" + s.mid(at + w.size()))
      .toStdString();
}

// Words right after the i-th one in the same sentence, for phrasal verbs:
// "FELL" -> {"FOR", "HER"}. Punctuation ends the run: "CONTRARY! IS" -> {}
std::vector<std::string> next_words(const std::vector<Word>& words, size_t i) {
  std::vector<std::string> out;
  auto ends_clean = [](const std::string& t) {
    return !t.empty() && Dictionary::strip_punct(t).size() == t.size();
  };
  for (size_t j = i + 1; j < words.size() && out.size() < 2; ++j) {
    if (words[j].context != words[i].context || !ends_clean(words[j - 1].text))
      break;
    out.push_back(Dictionary::strip_punct(words[j].text));
  }
  return out;
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
  // some of the surroundings too: the chat bubble, the panel
  const int pad_x = 3 * h, pad_y = 2 * h;
  cv::Rect roi = cv::Rect(cv::Point(x1 - pad_x, y1 - pad_y),
                          cv::Point(x2 + pad_x, y2 + pad_y)) &
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
                   Parser& parser, cv::Rect area,
                   std::optional<cv::Point> cursor) {

  static OllamaClient client;
  static YandexTranslator yandex;
  // with Yandex the local model is only a fallback: not kept in memory
  if (!yandex.configured())
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

  // Words grow when the area is recognized again: new ones are appended, so a
  // button keeps its index; `order` is their reading order
  auto words = std::make_shared<std::vector<Word>>(parser.get_words());
  auto order = std::make_shared<std::vector<size_t>>(words->size());
  std::iota(order->begin(), order->end(), 0);
  auto buttons = std::make_shared<std::vector<QPushButton*>>();

  // only the answer to the latest click is shown
  auto request_id = std::make_shared<std::atomic<int>>(0);
  auto selected = std::make_shared<QPointer<QPushButton>>();
  auto selected_word = std::make_shared<size_t>(0);

  static Pronunciations pronunciations(data_dir() + "/audio");
  static AnkiClient anki;

  popup->on_add_to_anki = [popup, selected_word, &screenshot, words] {
    const int shown = popup->shown();
    popup->set_anki_state(shown, "Adding…", false);

    AnkiNote note = popup->note();
    note.picture_base64 = picture_base64(screenshot, *words, *selected_word);

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

  // Translation of the word shown in the card and its sentence
  struct Request {
    std::string phrase, context;
  };
  auto last = std::make_shared<Request>();
  auto translate = [popup, request_id](Request r) {
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

    std::thread([r = std::move(r), id, request_id, deliver, target]() {
      const std::string sentence_en = OllamaClient::sentence_case(
          QString::fromStdString(r.context).simplified().toStdString());
      if (yandex.configured()) {
        try {
          const std::string html =
              yandex.translate_html(marked_html(sentence_en, r.phrase));
          deliver([html = QString::fromStdString(html)](Popup* p) {
            p->show_marked(html);
            p->finish_translation();
          });
          return;
        } catch (const std::exception& e) {
          // the local model translates instead
          std::cerr << e.what() << "\n";
        }
      }

      auto cancelled = [id, request_id] { return id != *request_id; };
      try {
        // a newer click cancels this request, so ollama doesn't queue stale
        // answers.
        // One request: on the CPU each takes several seconds. The word is
        // marked, so the model marks its translation too (see marked_md)
        client.translate(
            marked_md(sentence_en, r.phrase),
            [&](const std::string& piece) {
              deliver([piece](Popup* p) { p->append_translation(piece); });
            },
            cancelled);
        deliver([](Popup* p) { p->show_marked_final(); });

        // neither marked nor a dictionary translation found in the sentence:
        // the word alone
        bool found = false;
        QMetaObject::invokeMethod(
            qApp,
            [&] { found = target && id == *request_id && target->word_found(); },
            Qt::BlockingQueuedConnection);
        if (!found) {
          // lowercase: "ROGUE" gets an answer in caps
          std::string alone;
          client.translate(
              QString::fromStdString(r.phrase).toLower().toStdString(),
              [&](const std::string& piece) { alone += piece; }, cancelled);
          // the first of several: "ответственный, непредсказуемый, …"
          QString word = QString::fromStdString(alone)
                             .remove("**")
                             .section(QRegularExpression("[,;/\\n]"), 0, 0);
          deliver([word = word.trimmed().remove(QRegularExpression("[.!]+$"))](
                      Popup* p) { p->set_word_translation(word); });
        }
        deliver([](Popup* p) { p->finish_translation(); });
      } catch (const RequestCancelled&) {
      } catch (const std::exception& e) {
        deliver([msg = QString::fromStdString(e.what())](Popup* p) {
          p->set_translation_error(msg);
        });
      }
    }).detach();
  };

  // another translation of the sentence alone, with some randomness: the same
  // request at temperature 0 would give the same answer
  popup->on_retranslate = [popup, request_id, last] {
    popup->restart_sentence();
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
    std::thread([sentence_en = OllamaClient::sentence_case(
                     QString::fromStdString(last->context)
                         .simplified()
                         .toStdString()),
                 phrase = last->phrase, id, request_id, deliver] {
      try {
        client.translate(
            marked_md(sentence_en, phrase),
            [&](const std::string& piece) {
              deliver([piece](Popup* p) { p->append_sentence(piece); });
            },
            [id, request_id] { return id != *request_id; }, 0.8);
        deliver([](Popup* p) { p->finish_translation(); });
      } catch (const RequestCancelled&) {
      } catch (const std::exception& e) {
        deliver([msg = QString::fromStdString(e.what())](Popup* p) {
          p->set_translation_error(msg);
        });
      }
    }).detach();
  };

  // Recognizes the area again: a button over the cursor until a word is
  // clicked
  auto* improve = new QPushButton("Improve recognition", &window);
  improve->setObjectName("anki");
  improve->setStyleSheet(kPopupStyle);
  improve->setCursor(Qt::PointingHandCursor);
  improve->setToolTip("Recognize the area again and add the missed words");
  improve->adjustSize();
  {
    const QPoint at = cursor ? QPoint(cursor->x, cursor->y) / dpr
                             : QPoint(area.x + area.width / 2, area.y) / dpr;
    const int screen_w = screenshot.cols / dpr;
    improve->move(std::clamp(at.x() - improve->width() / 2, 0,
                             std::max(0, screen_w - improve->width())),
                  std::max(0, at.y() - improve->height() - 12));
  }

  auto on_word_click = [popup, request_id, selected, selected_word, words,
                        translate, last, improve,
                        &dict_error](QPushButton* btn, size_t i) {
    improve->hide();
    const std::string ocr_word = (*words)[i].text,
                      ocr_context = (*words)[i].context;
#ifdef __MY_LOG__
    std::cout << "------>" << ocr_word << " | " << ocr_context << "\n\n";
#endif

    if (*selected)
      (*selected)->setStyleSheet(kWordStyle);
    *selected = btn;
    *selected_word = i;
    btn->setStyleSheet(kSelectedWordStyle);

    // OCR misreads fixed before anything else: "INDEEP" -> "INDEED"
    const bool has_dict = dict_error.isEmpty();
    const std::string word =
        has_dict ? dictionary.correct_text(ocr_word) : ocr_word;
    const std::string context =
        has_dict ? dictionary.correct_text(ocr_context) : ocr_context;

    // "FELL" in "FELL FOR HER" is looked up and translated as "fall for"
    std::string phrase = Dictionary::strip_punct(word);
    std::optional<DictEntry> entry;
    if (has_dict) {
      auto next = next_words(*words, i);
      for (auto& w : next)
        w = dictionary.correct_word(w);
      if (auto phrasal = dictionary.lookup_phrasal(word, next)) {
        entry = std::move(phrasal->first);
        for (size_t k = 0; k < phrasal->second; ++k)
          phrase += " " + next[k];
      } else {
        entry = dictionary.lookup(word);
      }
    }
    popup->show_word(btn->geometry(), phrase, context, entry, dict_error);

    *last = {phrase, context};
    translate(*last);
  };

  auto place_button = [dpr](QPushButton* btn, const Word& w) {
    auto [x1, y1, x2, y2] = w.box;
    btn->setGeometry(x1 / dpr - 3, y1 / dpr - 3, (x2 - x1) / dpr + 7,
                     (y2 - y1) / dpr + 7);
  };
  auto add_button = [&window, popup, buttons, words, place_button,
                     on_word_click](size_t i) {
    QPushButton* btn = new QPushButton("", &window);
    place_button(btn, (*words)[i]);
    btn->setStyleSheet(kWordStyle);
    QObject::connect(btn, &QPushButton::clicked,
                     [btn, i, on_word_click] { on_word_click(btn, i); });
    btn->show();
    // the card stays on top of the words
    btn->stackUnder(popup);
    buttons->push_back(btn);
  };
  for (size_t i = 0; i < words->size(); ++i)
    add_button(i);
  // above the word boxes it may cover
  improve->raise();

  // Recognizes the area again at other settings and adds what the first pass
  // missed: 3x for small text, then straightened italics (comics). The card
  // is shown again if the sentence of its word has changed. `report` gets the
  // result: "+3 words", or the error.
  using Report = std::function<void(const QString& result, const QString& error)>;
  auto improve_ocr = [popup, &parser, &screenshot, area, words, order, buttons,
                      place_button, add_button, selected, selected_word,
                      on_word_click](Report report) {
    std::thread([=, &parser, &screenshot] {
      std::vector<Word> extra;
      QString error;
      try {
        for (Parser::Options o : {Parser::Options{3.0, 0.0},
                                  Parser::Options{2.0, 0.15}}) {
          parser.process(screenshot(area), area.tl(), o);
          const auto& w = parser.get_words();
          extra.insert(extra.end(), w.begin(), w.end());
        }
      } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
      }
      QMetaObject::invokeMethod(
          qApp,
          [=, extra = std::move(extra)]() mutable {
            if (!error.isEmpty()) {
              report("OCR failed", error);
              return;
            }
            const size_t before = words->size(), sel = *selected_word;
            const std::string old_text = (*words)[sel].text,
                              old_context = (*words)[sel].context;
            int added = 0;
            for (size_t i : merge_words(*words, *order, std::move(extra))) {
              if (i >= before) {
                add_button(i);
                ++added;
              } else {
                place_button((*buttons)[i], (*words)[i]);
              }
            }
            if (popup->isVisible() && *selected &&
                ((*words)[sel].text != old_text ||
                 (*words)[sel].context != old_context))
              on_word_click(selected->data(), sel);
            report(added ? QString("+%1 words").arg(added)
                         : QString("Nothing new"),
                   "");
          },
          Qt::QueuedConnection);
    }).detach();
  };

  QObject::connect(improve, &QPushButton::clicked, [improve, improve_ocr] {
    improve->setEnabled(false);
    improve->setText("Recognizing…");
    improve->adjustSize();
    QPointer<QPushButton> target = improve;
    improve_ocr([target](const QString& result, const QString& error) {
      if (!target)
        return;
      target->setText(result);
      if (!error.isEmpty())
        target->setToolTip(error);
      target->setEnabled(true);
      target->adjustSize();
      target->raise();
    });
  });

  popup->on_improve_ocr = [popup, improve_ocr] {
    popup->set_improve_state("Recognizing…", false, "");
    QPointer<Popup> target = popup;
    improve_ocr([target](const QString& result, const QString& error) {
      if (target)
        target->set_improve_state(result, true,
                                  error.isEmpty() ? "Recognize the area again"
                                                  : error);
    });
  };

  window.show();

  return app.exec();
}
