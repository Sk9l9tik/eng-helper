cmake_minimum_required(VERSION 3.22)
project(OpenCV_Qt_Tesseract_Test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

include(FetchContent)

# ---------------- OpenCV ----------------
find_package(OpenCV REQUIRED)
if(NOT OpenCV_FOUND)
    message(FATAL_ERROR "OpenCV not found! Please install it first.")
endif()
include_directories(${OpenCV_INCLUDE_DIRS})

# ---------------- Tesseract ----------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(TESSERACT REQUIRED tesseract)
if(NOT TESSERACT_FOUND)
    message(FATAL_ERROR "Tesseract not found! Please install it first.")
endif()
include_directories(${TESSERACT_INCLUDE_DIRS})

# ---------------- Qt6 ----------------
find_package(Qt6 6.5 COMPONENTS Widgets Gui Core QUIET)

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found. Fetching via FetchContent...")
    set(FETCHCONTENT_QUIET FALSE)

    FetchContent_Declare(
        qt
        URL https://download.qt.io/official_releases/qt/6.5/6.5.0/single/qt-everywhere-src-6.5.0.tar.xz
    )
    FetchContent_MakeAvailable(qt)

    set(CMAKE_PREFIX_PATH ${qt_SOURCE_DIR})
    find_package(Qt6 6.5 COMPONENTS Widgets Gui Core REQUIRED)
endif()

# ---------------- Executable ----------------
set(SOURCES
    main.cc
    UI.cc
    Parser.h
)

add_executable(opencv_test ${SOURCES})

# ---------------- Link Libraries ----------------
target_link_libraries(opencv_test
    PRIVATE
        ${OpenCV_LIBS}
        ${TESSERACT_LIBRARIES}
        Qt6::Widgets
        Qt6::Gui
        Qt6::Core
)

# ---------------- Enable MOC for Qt ----------------
set_target_properties(opencv_test PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)
