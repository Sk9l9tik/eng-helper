#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <future>
#include <memory>
#include <optional>
#include <string_view>

#include "UI.h"

cv::Mat make_screenshot() {
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen("grim -t ppm -", "r"),
                                             pclose);
  if (!pipe)
    throw std::runtime_error("Failed to run grim");

  std::vector<uchar> buf;
  uchar chunk[1 << 16];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), pipe.get())) > 0)
    buf.insert(buf.end(), chunk, chunk + n);

  return cv::imdecode(buf, cv::IMREAD_COLOR);
}

// Cursor position on the screen, from Hyprland
std::optional<cv::Point> cursor_pos() {
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(
      popen("hyprctl cursorpos 2>/dev/null", "r"), pclose);
  int x, y;
  if (!pipe || fscanf(pipe.get(), "%d, %d", &x, &y) != 2)
    return std::nullopt;
  return cv::Point(x, y);
}

// Only the text around the cursor is recognized: recognition time grows with
// the amount of text, and a full screen of it takes several times longer. The
// whole screenshot is still taken (it is cheap) for the Anki picture.
// Assumes scale 1: hyprctl gives logical coordinates, grim physical pixels.
constexpr int kOcrSize = 700;

cv::Rect ocr_area(const cv::Mat& screen, std::optional<cv::Point> cursor) {
  const cv::Rect all(0, 0, screen.cols, screen.rows);
  if (!cursor)
    return all;
  cv::Rect area(cursor->x - kOcrSize / 2, cursor->y - kOcrSize / 2, kOcrSize,
                kOcrSize);
  // near an edge the square moves inside rather than shrinks
  area.x = std::clamp(area.x, 0, std::max(0, screen.cols - kOcrSize));
  area.y = std::clamp(area.y, 0, std::max(0, screen.rows - kOcrSize));
  area &= all;

  // Text lines the square cuts are taken whole, so words are not cut in half
  // and the sentence around the clicked word is all there. Letters and words
  // of a line are merged into one blob; tall blobs are pictures, not lines.
  cv::Mat gray, text;
  cv::cvtColor(screen, gray, cv::COLOR_BGR2GRAY);
  cv::threshold(Parser::dark_text_on_light(gray), text, 255 - 40, 255,
                cv::THRESH_BINARY_INV);
  cv::dilate(text, text,
             cv::getStructuringElement(cv::MORPH_RECT, cv::Size(25, 3)));
  cv::Mat labels, stats, centroids;
  const int n = cv::connectedComponentsWithStats(text, labels, stats, centroids);
  constexpr int max_line_height = 100;
  cv::Rect out = area;
  for (int i = 1; i < n; ++i) {
    const cv::Rect blob(stats.at<int>(i, cv::CC_STAT_LEFT),
                        stats.at<int>(i, cv::CC_STAT_TOP),
                        stats.at<int>(i, cv::CC_STAT_WIDTH),
                        stats.at<int>(i, cv::CC_STAT_HEIGHT));
    if (blob.height <= max_line_height && (blob & area).area() > 0)
      out |= blob;
  }
  // a little room around the letters
  constexpr int pad = 4;
  out.x -= pad;
  out.y -= pad;
  out.width += 2 * pad;
  out.height += 2 * pad;
  return out & all;
}

int main(int argc, char** argv) {

  // Parser runs a tesseract instance per core (see Parser::process); OpenMP
  // threads inside each instance oversubscribe the cores: recognition takes
  // ~1.1 s instead of ~0.3 s. libgomp reads OMP_THREAD_LIMIT when it is loaded,
  // before main, so setenv alone has no effect: the process restarts itself
  // with the variable set. If exec fails, it just runs slower.
  if (const char* omp = getenv("OMP_THREAD_LIMIT");
      !omp || std::string_view(omp) != "1") {
    setenv("OMP_THREAD_LIMIT", "1", 1);
    execv("/proc/self/exe", argv);
  }

  // tesseract models load while the screen is captured, and recognition runs
  // while Qt starts
  auto parser = std::async(std::launch::async, [] {
    return std::make_unique<Parser>(TESSDATA_FAST_DIR);
  });

  cv::Mat img = make_screenshot();

  const auto cursor = cursor_pos();
  cv::Rect area;
  auto recognized = std::async(std::launch::async, [&] {
    auto p = parser.get();
    area = ocr_area(img, cursor);
    p->process(img(area), area.tl());
    p->save_detected_words("/tmp/all_words.txt");
    return p;
  });

  QApplication app(argc, argv);
  // Kvantum (QT_STYLE_OVERRIDE) blurs translucent windows and hides what's
  // under the overlay
  QApplication::setStyle("Fusion");

  auto p = recognized.get();
  draw_interface(app, img, *p, area, cursor);

  return 0;
}
