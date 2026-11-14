cmake_minimum_required(VERSION 3.21)
project(QtProcessTemplate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
include(cmake/MonitoringTargets.cmake)

# WIN32 keeps the linker on /subsystem:windows; without it the GUI app is
# linked as a console app and opens a stray console window next to itself.
qt_add_executable(QtProcessTemplate WIN32
    src/main.cpp
    src/MonitoringClient.cpp
    src/SetupLauncher.cpp
    src/ServiceSnapshotMerge.cpp
    src/SettingsPage.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/ProcessEntry.h
    src/ProcessModel.h
    src/ProcessModel.cpp
    src/ProcessFilterProxy.h
    src/ProcessFilterProxy.cpp
    src/CellButtonDelegate.h
    src/CellButtonDelegate.cpp
    src/CellTextCache.h
    src/CellTextCache.cpp
    src/FileIconCache.h
    src/FileIconCache.cpp
    src/ProcessPage.h
    src/ProcessPage.cpp
    src/ProcessMonitor.h
    src/ProcessMonitor.cpp
    src/ProcessProviderWin.h
    src/ProcessProviderWin.cpp
    src/ProcessDetailsDialog.h
    src/ProcessDetailsDialog.cpp
    src/PerfGraph.h
    src/PerfGraph.cpp
    src/PerformancePage.h
    src/PerformancePage.cpp
    src/PerformanceMonitor.h
    src/PerformanceMonitor.cpp
    src/AudioDevices.h
    src/AudioDevices.cpp
    src/ProcessActions.h
    src/ProcessActions.cpp
    src/ListViewStyle.h
    src/HandleSearch.h
    src/HandleSearch.cpp
    src/SearchPage.h
    src/SearchPage.cpp
    resources/resources.qrc
)

set_target_properties(QtProcessTemplate PROPERTIES OUTPUT_NAME QTaskManager)

target_link_libraries(QtProcessTemplate PRIVATE Qt6::Widgets Qt6::Svg MonitoringCore)

target_compile_definitions(QtProcessTemplate PRIVATE UNICODE _UNICODE)

# A PDB for Release as well, so a profiler (WPA, xperf) can name our own
# frames in a trace of the optimized build. /OPT:REF and /OPT:ICF are what
# Release links with anyway; /DEBUG alone would switch them off.
if (MSVC)
    target_link_options(QtProcessTemplate PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
    target_compile_options(QtProcessTemplate PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(QtProcessTemplate PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

if (WIN32)
    target_link_libraries(QtProcessTemplate PRIVATE psapi advapi32 shell32 ntdll pdh dxgi gdi32 ole32)
    # Embeds app.ico so the .exe itself has an icon; setWindowIcon only
    # covers the window once it is running.
    target_sources(QtProcessTemplate PRIVATE resources/app.rc)
endif()

include(cmake/MonitoringPackage.cmake)
if(BUILD_TESTING)
    get_target_property(GUI_TEST_SOURCES QtProcessTemplate SOURCES)
    list(REMOVE_ITEM GUI_TEST_SOURCES src/main.cpp)
    add_executable(GuiTests test/monitoring/GuiTests.cpp ${GUI_TEST_SOURCES})
    target_link_libraries(GuiTests PRIVATE MonitoringCore Qt6::Widgets Qt6::Svg psapi advapi32 shell32 ntdll pdh dxgi gdi32 ole32)
    add_test(NAME GuiTests COMMAND GuiTests)
    set_tests_properties(GuiTests PROPERTIES LABELS monitoring-unit TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=C:/Qt/6.11.0/msvc2022_64/bin;$ENV{PATH}")
endif()
