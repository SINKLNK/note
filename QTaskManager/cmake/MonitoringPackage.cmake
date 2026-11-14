find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin" "C:/Qt/6.11.0/msvc2022_64/bin" REQUIRED)
set(MONITORING_PACKAGE_DIR "${CMAKE_BINARY_DIR}/monitoring-package" CACHE PATH "Runnable monitoring release directory")
add_custom_target(monitoring-package
 COMMAND ${CMAKE_COMMAND} -E make_directory "${MONITORING_PACKAGE_DIR}"
 COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:QtProcessTemplate>" "$<TARGET_FILE:QTaskManagerSetup>" "$<TARGET_FILE:QTaskManagerService>" "${MONITORING_PACKAGE_DIR}"
 COMMAND "${WINDEPLOYQT_EXECUTABLE}" --release --no-translations --no-opengl-sw --no-system-d3d-compiler "${MONITORING_PACKAGE_DIR}/QTaskManager.exe"
 DEPENDS QtProcessTemplate QTaskManagerSetup QTaskManagerService VERBATIM)
