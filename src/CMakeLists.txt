set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(KI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/keyboard_inspector")

file(GLOB SOURCES
    "${KI_SOURCE_DIR}/main.cpp"
    "${KI_SOURCE_DIR}/core/model.cpp"
    "${KI_SOURCE_DIR}/core/analysis.cpp"
    "${KI_SOURCE_DIR}/core/kbi.cpp"
    "${KI_SOURCE_DIR}/core/recording.cpp"
    "${KI_SOURCE_DIR}/core/keynames.cpp"
    "${KI_SOURCE_DIR}/core/tetrio.cpp"
    "${KI_SOURCE_DIR}/core/logger.cpp"
    "${KI_SOURCE_DIR}/capture/base.cpp"
    "${KI_SOURCE_DIR}/capture/evdev_backend.cpp"
    "${KI_SOURCE_DIR}/capture/usb_info.cpp"
    "${KI_SOURCE_DIR}/ui/theme.cpp"
    "${KI_SOURCE_DIR}/ui/chart.cpp"
    "${KI_SOURCE_DIR}/ui/timeline.cpp"
    "${KI_SOURCE_DIR}/ui/main_window.cpp"
    "${KI_SOURCE_DIR}/ui/dialogs.cpp"
)

include_directories("${KI_SOURCE_DIR}")

add_executable(keyboard-inspector ${SOURCES})

target_link_libraries(keyboard-inspector PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Concurrent
    Qt6::Network
    evdev
)

if(TARGET kissfft)
    target_link_libraries(keyboard-inspector PRIVATE kissfft)
    target_compile_definitions(keyboard-inspector PRIVATE USE_KISSFFT kiss_fft_scalar=double)
endif()

target_compile_definitions(keyboard-inspector PRIVATE
    APP_NAME="Keyboard Inspector Linux"
    APP_VERSION="0.1.0"
)

target_link_options(keyboard-inspector PRIVATE -rdynamic)

install(TARGETS keyboard-inspector RUNTIME DESTINATION bin)
