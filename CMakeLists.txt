cmake_minimum_required(VERSION 3.16)

project(Thunder VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick)

qt_standard_project_setup(REQUIRES 6.10)

# Content of downloaded design from Figma to Qt or similar
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/importedcontent/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/importedcontent)
endif()

qt_add_executable(appThunder
    main.cpp
    ps3_texpack.h
    ps3_texpack.c
    x360_texpack.h
    x360_texpack.c
    wii_texpack.h
    wii_texpack.c
    tszip_reader.h
    tszip_reader.c
    tszip_writer.h
    tszip_writer.c
    tinflate.h
    tinflate.c
    dds_preview.h
    dds_preview.c
    dds_image_provider.h
    dds_image_provider.cpp
    folder_watcher.h
    folder_watcher.cpp
    texture_packer_bridge.h
    texture_packer_bridge.cpp
)

qt_add_qml_module(appThunder
    URI Thunder
    QML_FILES
        Main.qml
        TexturePacker.qml
    RESOURCES
        fonts/Ubuntu-Regular.ttf
        fonts/segoeui.ttf
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(appThunder PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.appThunder
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appThunder
    PRIVATE Qt6::Quick
)

# ps3_texpack.c's PCA-based DXT1/DXT5 encoder uses round()/floor()/sqrt()/
# fabs(). On Linux/macOS these need explicit -lm; on Windows/MinGW they're
# already part of the C runtime, so only link libm on non-Windows.
if (NOT WIN32)
    target_link_libraries(appThunder PRIVATE m)
endif()

install(TARGETS appThunder
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
