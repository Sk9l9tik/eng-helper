cmake_minimum_required(VERSION 3.10)
project(OpenCVTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# OpenCV
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# Tesseract
find_package(PkgConfig REQUIRED)
pkg_check_modules(TESSERACT REQUIRED tesseract)
include_directories(${TESSERACT_INCLUDE_DIRS})

add_executable(opencv_test main.cc)
target_link_libraries(opencv_test ${OpenCV_LIBS} ${TESSERACT_LIBRARIES})
