cmake_minimum_required(VERSION 3.21)
project(hype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Like Qt's own modules, Hype never throws or catches. Without unwinding tables
# and with link-time optimization, the installed binary is about a quarter smaller.
add_compile_options(-fno-exceptions)
add_compile_definitions(QT_NO_EXCEPTIONS)

# A Qt from Qt's installer unpacked under build/qt/ wins over an older system
# Qt, newest first, unless CMAKE_PREFIX_PATH picks another.
if(NOT CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
  file(GLOB bundled_qt LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/build/qt/*/gcc_64)
  list(SORT bundled_qt COMPARE NATURAL ORDER DESCENDING)
  set(CMAKE_PREFIX_PATH ${bundled_qt})
endif()

find_package(Qt6 6.9 REQUIRED COMPONENTS
  Core Gui Qml Quick QuickControls2 Multimedia Concurrent DBus Svg Widgets Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp libwebpdemux)
find_package(ZLIB REQUIRED)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB hype_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM hype_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(hype_core STATIC ${hype_sources})
target_include_directories(hype_core PUBLIC src)
target_link_libraries(hype_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia Qt6::Concurrent
  Qt6::DBus Qt6::Svg PkgConfig::WEBP ZLIB::ZLIB)
target_compile_options(hype_core PUBLIC -Wall -Wextra)
set_target_properties(hype_core PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)

add_executable(hype src/main.cpp src/resources.qrc)
target_link_libraries(hype PRIVATE hype_core)
set_target_properties(hype PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)

enable_testing()

# The tests read exports back with Qt Pdf, which ships in qt6-webengine, so
# building the app alone doesn't need it.
find_package(Qt6 6.9 COMPONENTS Pdf)
if(TARGET Qt6::Pdf)
  # The window-level tests load the same QML as the app, so they embed it too.
  add_executable(hype_tests tests/tests.cpp src/resources.qrc)
  target_link_libraries(hype_tests PRIVATE hype_core Qt6::Widgets Qt6::Pdf Qt6::Test)
  set_target_properties(hype_tests PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
  add_test(NAME hype_tests COMMAND hype_tests)
  # Hype draws its own controls, so the tests keep Qt's platform theme and style out of it.
  set_tests_properties(hype_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORMTHEME=generic;QT_STYLE_OVERRIDE=Fusion")
else()
  message(WARNING "Qt Pdf was not found, so the tests can't be built; install qt6-webengine.")
endif()

# The commands, exports and shutdown, driven through build/hype as an agent would.
# test_trials covers the trial import tools, which need their own Python packages.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME hype_commands
    COMMAND ${Python3_EXECUTABLE} -m unittest test_cli test_export test_shutdown
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
endif()
