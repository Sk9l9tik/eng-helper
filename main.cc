#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <future>
#include <memory>
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

  auto recognized = std::async(std::launch::async, [&] {
    auto p = parser.get();
    p->process(img);
    p->save_detected_words("all_words.txt");
    return p;
  });

  QApplication app(argc, argv);
  // Kvantum (QT_STYLE_OVERRIDE) blurs translucent windows and hides what's
  // under the overlay
  QApplication::setStyle("Fusion");

  auto p = recognized.get();
  draw_interface(app, img, p->get_words(), p->get_blocks());

  return 0;
}
