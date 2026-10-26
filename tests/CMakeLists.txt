set(TEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB TEST_SOURCES
    "${TEST_SOURCE_DIR}/test_*.cpp"
)

file(GLOB CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/capture/*.cpp"
)

add_executable(keyboard-inspector-tests ${TEST_SOURCES} ${CORE_SOURCES})
target_link_libraries(keyboard-inspector-tests PRIVATE
    Qt6::Core
    Catch2::Catch2WithMain
    evdev
)
if(TARGET kissfft)
    target_link_libraries(keyboard-inspector-tests PRIVATE kissfft)
    target_compile_definitions(keyboard-inspector-tests PRIVATE USE_KISSFFT kiss_fft_scalar=double)
endif()
target_include_directories(keyboard-inspector-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector"
)
target_compile_definitions(keyboard-inspector-tests PRIVATE
    APP_NAME="Keyboard Inspector Linux"
    APP_VERSION="0.1.0"
)
include(CTest)
add_test(NAME keyboard-inspector-tests COMMAND keyboard-inspector-tests)

set(CMAKE_AUTOMOC ON)

set(UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/ui/theme.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/ui/chart.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/ui/timeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/ui/main_window.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector/ui/dialogs.cpp"
)

add_executable(keyboard-inspector-ui-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_freeze_test.cpp"
    ${CORE_SOURCES}
    ${UI_SOURCES}
)
set_target_properties(keyboard-inspector-ui-tests PROPERTIES AUTOMOC ON)
target_link_libraries(keyboard-inspector-ui-tests PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Concurrent
    Qt6::Network
    Catch2::Catch2WithMain
    evdev
)
if(TARGET kissfft)
    target_link_libraries(keyboard-inspector-ui-tests PRIVATE kissfft)
    target_compile_definitions(keyboard-inspector-ui-tests PRIVATE USE_KISSFFT kiss_fft_scalar=double)
endif()
target_include_directories(keyboard-inspector-ui-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/keyboard_inspector"
)
target_compile_definitions(keyboard-inspector-ui-tests PRIVATE
    APP_NAME="Keyboard Inspector Linux"
    APP_VERSION="0.1.0"
)
add_test(NAME keyboard-inspector-ui-tests COMMAND keyboard-inspector-ui-tests)
set_tests_properties(keyboard-inspector-ui-tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
