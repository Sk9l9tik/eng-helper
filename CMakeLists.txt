cmake_minimum_required(VERSION 3.22)
set(PROJECT_NAME LookUpper)
project(${PROJECT_NAME})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_COMPILER g++)

set(CMAKE_AUTOMOC ON) 
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# include(FetchContent)

# ---------------- OpenCV ----------------
# only the modules in use: linking all of them loads ~350 shared libraries (VTK, HDF5, ffmpeg, ...) at every start
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(PkgConfig REQUIRED)
if(NOT OpenCV_FOUND)
    message(FATAL_ERROR "OpenCV not found! Please install it first.")
endif()

# target_link_options(${PROJECT_NAME} PRIVATE
#     -Wl,-rpath,/opt/ffmpeg-6.1/lib
# )

# include_directories(${OpenCV_INCLUDE_DIRS})

# ---------------- Tesseract ----------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(TESSERACT REQUIRED tesseract)
if(NOT TESSERACT_FOUND)
    message(FATAL_ERROR "Tesseract not found! Please install it first.")
endif()
# include_directories(${TESSERACT_INCLUDE_DIRS})

# fast (int8) model: ~1.5x faster than the default eng.traineddata with the same results on screenshots
set(TESSDATA_FAST_DIR ${CMAKE_BINARY_DIR}/tessdata)
if(NOT EXISTS ${TESSDATA_FAST_DIR}/eng.traineddata)
    file(DOWNLOAD
        https://github.com/tesseract-ocr/tessdata_fast/raw/main/eng.traineddata
        ${TESSDATA_FAST_DIR}/eng.traineddata
        STATUS TESSDATA_DOWNLOAD_STATUS)
    list(GET TESSDATA_DOWNLOAD_STATUS 0 TESSDATA_DOWNLOAD_CODE)
    if(NOT TESSDATA_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE ${TESSDATA_FAST_DIR}/eng.traineddata)
        message(FATAL_ERROR "Failed to download tessdata_fast eng.traineddata: ${TESSDATA_DOWNLOAD_STATUS}")
    endif()
endif()

# ---------------- Qt6 ----------------
find_package(Qt6 COMPONENTS Widgets Gui Core REQUIRED Network)
# if(NOT Qt6_FOUND)
#     message(STATUS "Qt6 not found. Fetching via FetchContent...")
#     set(FETCHCONTENT_QUIET FALSE)
#     FetchContent_Declare(
#         qt
#         URL https://download.qt.io/official_releases/qt/6.5/6.5.0/single/qt-everywhere-src-6.5.0.tar.xz
#     )
#     FetchContent_MakeAvailable(qt)
#     set(CMAKE_PREFIX_PATH ${qt_SOURCE_DIR})
#     find_package(Qt6 6.5 COMPONENTS Widgets Gui Core REQUIRED)
# endif()

# Убираем macOS-флаг, если он есть

# ---------------- LayerShellQt ----------------
find_package(LayerShellQt REQUIRED)

# ---------------- zlib (dictionary index) ----------------
find_package(ZLIB REQUIRED)

# ---------------- libcurl (ollama, AnkiConnect, pronunciations) ----------------
find_package(CURL REQUIRED)






# ---------------- Executable ----------------
set(SOURCES
    main.cc
    UI.h
    UI.cc
    Text.h
    Parser.h
    Dictionary.h
    LLM.h
    Yandex.h
    Anki.h
    Audio.h
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE TESSDATA_FAST_DIR="${TESSDATA_FAST_DIR}/")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${TESSERACT_INCLUDE_DIRS}
)

# ---------------- Link Libraries ----------------
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        opencv_core
        opencv_imgproc
        opencv_imgcodecs
        ${TESSERACT_LIBRARIES}
        Qt6::Widgets
        Qt6::Gui
        Qt6::Core
        Qt6::Network
        LayerShellQt::Interface
        ZLIB::ZLIB
        CURL::libcurl
        # VTK::CommonCore
        # VTK::RenderingCore
        # VTK::RenderingOpenGL2
)

