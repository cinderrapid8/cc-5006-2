cmake_minimum_required(VERSION 3.21)
project(SportReviewDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale libswresample libavfilter)

enable_testing()

# Tests run against the user-local sysroot and the headless Qt platform.
set(SYSROOT /workspace/.sysroot/root)
set(TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "LD_LIBRARY_PATH=${SYSROOT}/lib/aarch64-linux-gnu:${SYSROOT}/usr/lib/aarch64-linux-gnu:${SYSROOT}/usr/lib/aarch64-linux-gnu/pulseaudio:${SYSROOT}/usr/lib/aarch64-linux-gnu/blas:${SYSROOT}/usr/lib/aarch64-linux-gnu/lapack:${SYSROOT}/usr/lib"
    "QT_PLUGIN_PATH=${SYSROOT}/usr/lib/aarch64-linux-gnu/qt6/plugins")

add_library(sr_core STATIC
    src/core/Types.cpp
    src/core/VideoDecoder.cpp
    src/core/Project.cpp
    src/core/Exporter.cpp
)
target_include_directories(sr_core PUBLIC src)
target_link_libraries(sr_core PUBLIC Qt6::Core Qt6::Gui PkgConfig::FFMPEG)

add_executable(sportreview
    src/ui/main.cpp
    src/ui/MainWindow.cpp
    src/ui/ExportDialog.cpp
)
target_link_libraries(sportreview PRIVATE sr_core Qt6::Widgets)

add_executable(srt_probe tools/probe_main.cpp)
target_link_libraries(srt_probe PRIVATE sr_core)

add_executable(srt_genclip tools/genclip_main.cpp)
target_link_libraries(srt_genclip PRIVATE sr_core Qt6::Gui)

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE sr_core Qt6::Test)
add_test(NAME core COMMAND test_core)
set_tests_properties(core PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_ui
    tests/test_ui.cpp
    src/ui/MainWindow.cpp
    src/ui/ExportDialog.cpp
)
target_link_libraries(test_ui PRIVATE sr_core Qt6::Widgets Qt6::Test)
add_test(NAME ui COMMAND test_ui)
set_tests_properties(ui PROPERTIES ENVIRONMENT "${TEST_ENV}")
