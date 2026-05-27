cmake_minimum_required(VERSION 3.20)
project(qt_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(Libsumo REQUIRED)

add_executable(qt_cpp
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/NetworkView.cpp
    src/NetworkView.h
    src/Camera.cpp
    src/Camera.h
    src/gl/Shader.cpp
    src/gl/Shader.h
    src/layers/EdgeColorLayer.cpp
    src/layers/EdgeColorLayer.h
    src/layers/NetworkLayer.cpp
    src/layers/NetworkLayer.h
    src/layers/PersonLayer.cpp
    src/layers/PersonLayer.h
    src/layers/PolygonLayer.cpp
    src/layers/PolygonLayer.h
    src/layers/RailLayer.cpp
    src/layers/RailLayer.h
    src/layers/StopLineLayer.cpp
    src/layers/StopLineLayer.h
    src/layers/TLSLayer.cpp
    src/layers/TLSLayer.h
    src/layers/VehicleLayer.cpp
    src/layers/VehicleLayer.h
    src/sim/NetworkGeometry.cpp
    src/sim/NetworkGeometry.h
    src/sim/SimSnapshot.h
    src/sim/SimWorker.cpp
    src/sim/SimWorker.h
)

target_include_directories(qt_cpp PRIVATE src)
target_link_libraries(qt_cpp PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Libsumo::Libsumo
)

# So the shipped libsumocpp.so is found at runtime without LD_LIBRARY_PATH.
set_target_properties(qt_cpp PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${LIBSUMO_LIBRARY_DIR}"
)
