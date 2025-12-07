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
find_package(OpenCV REQUIRED)
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

# ---------------- VTK ----------------
find_package(HDF5 REQUIRED COMPONENTS C HL)
if(NOT HDF5_FOUND)
    message(FATAL_ERROR "HDF5 not found!")
endif()





# ---------------- Executable ----------------
set(SOURCES
    main.cc
    UI.h
    UI.cc
    Parser.h
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${TESSERACT_INCLUDE_DIRS}
)

# ---------------- Link Libraries ----------------
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${OpenCV_LIBS}
        ${TESSERACT_LIBRARIES}
        Qt6::Widgets
        Qt6::Gui
        Qt6::Core
        Qt6::Network
        # VTK::CommonCore
        # VTK::RenderingCore
        # VTK::RenderingOpenGL2
        HDF5::HDF5
)

