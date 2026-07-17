cmake_minimum_required(VERSION 3.22)
project(PickMoji VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(WIN32)
    # windows.h defines min/max as macros, which breaks std::max / std::min.
    add_compile_definitions(NOMINMAX)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(PickMoji WIN32
    main.cpp
    app_controller.cpp
    app_controller.h
    emoji_canvas.cpp
    emoji_canvas.h
    emoji_repository.cpp
    emoji_repository.h
    picker_window.cpp
    picker_window.h
    single_instance.cpp
    single_instance.h
    update_checker.cpp
    update_checker.h
    usage_store.cpp
    usage_store.h
    windows_integration.cpp
    windows_integration.h
    emoji_keywords.h
    resources.qrc
)

target_link_libraries(PickMoji PRIVATE Qt6::Widgets Qt6::Network)
# The self-updater compares against this and reports it as the running version.
target_compile_definitions(PickMoji PRIVATE PICKMOJI_VERSION="${PROJECT_VERSION}")

file(GLOB FLAG_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/flags/*.png"
)
qt_add_resources(PickMoji "flag_assets"
    PREFIX "/flags"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/flags"
    FILES ${FLAG_ASSETS}
)

if(WIN32)
    # ole32/oleaut32: UI Automation is COM (CoCreateInstance + SAFEARRAY), used to
    # locate the caret in apps that have no classic Win32 caret.
    target_link_libraries(PickMoji PRIVATE user32 ole32 oleaut32)

    # Embed the multi-resolution application icon so Explorer, the taskbar and
    # the installer show it instead of the generic Qt executable icon.
    set(PICKMOJI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/PickMoji.ico")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(PickMoji PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()

if(MSVC)
    target_compile_options(PickMoji PRIVATE /W4 /permissive-)
endif()

install(TARGETS PickMoji
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md TWEMOJI-LICENSE-GRAPHICS.txt DESTINATION .)
# Drop-in search-keyword packs: the app loads every *.json in <exe dir>/keywords,
# so ship the folder (docs + template) next to the executable.
install(DIRECTORY keywords DESTINATION bin)

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET PickMoji
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS
            # Keep the TLS plugin: the updater needs HTTPS to api.github.com
            # (Schannel backend). PNG is built into Qt, so imageformats can go.
            "--skip-plugin-types" "generic,iconengines,imageformats,networkinformation"
            "--no-translations"
            "--no-opengl-sw"
    )
    install(SCRIPT ${deploy_script})
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(PickMojiSmokeTest
        tests/smoke_test.cpp
        emoji_repository.cpp
        emoji_repository.h
        windows_integration.cpp
        windows_integration.h
        emoji_keywords.h
        resources.qrc
    )
    target_include_directories(PickMojiSmokeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(PickMojiSmokeTest PRIVATE Qt6::Widgets)
    qt_add_resources(PickMojiSmokeTest "test_flag_assets"
        PREFIX "/flags"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/flags"
        FILES ${FLAG_ASSETS}
    )
    if(WIN32)
        target_link_libraries(PickMojiSmokeTest PRIVATE user32 ole32 oleaut32)
    endif()
    if(MSVC)
        target_compile_options(PickMojiSmokeTest PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME pickmoji_smoke COMMAND PickMojiSmokeTest)
    get_target_property(QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION_RELEASE)
    get_filename_component(QT_RUNTIME_DIRECTORY "${QT_CORE_RUNTIME}" DIRECTORY)
    set_tests_properties(pickmoji_smoke PROPERTIES
        TIMEOUT 12
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_RUNTIME_DIRECTORY}"
    )
endif()
