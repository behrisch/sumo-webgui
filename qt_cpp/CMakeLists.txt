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

# for windows install QT via
# > winget install miurahr.aqtinstall
# > mkdir %SUMO_LIBRARIES%\Qt6; cd %SUMO_LIBRARIES%\Qt6
# > aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --modules qtshadertools

# qt_cpp deserializes the shared eCAL network-geometry cache published by
# ecal_deck/sumo_ecal_publisher.py.  The generated sumo_ecal.pb.h comes from
# libsumocpp's build tree via FindLibsumo.cmake; we only need libprotobuf at
# link time for the deserializer.  See "Shared `.pb` network cache +
# early-render" in PLAN.md for the design.

# Check if libraries have to be found, depending on SUMO_LIBRARIES
set(SUMO_LIBRARIES "$ENV{SUMO_LIBRARIES}" CACHE PATH "Location of SUMOLibraries dependencies")

# some user place SUMOLibraries in the same SUMO folder
if (NOT SUMO_LIBRARIES AND EXISTS "${CMAKE_SOURCE_DIR}/../../SUMOLibraries")
    set(SUMO_LIBRARIES "${CMAKE_CURRENT_SOURCE_DIR}/../../SUMOLibraries")
endif ()
if (SUMO_LIBRARIES AND WIN32)
    file(GLOB ECAL_PATH "${SUMO_LIBRARIES}/eCAL*")
    file(GLOB QT6_PATH "${SUMO_LIBRARIES}/Qt*/*/msvc*")
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${ECAL_PATH};${QT6_PATH}")
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets ShaderTools)
find_package(eCAL QUIET CONFIG)
find_package(Protobuf QUIET)

list(APPEND CMAKE_PREFIX_PATH "$ENV{SUMO_HOME}/cmvs/Release")
find_package(SUMO)

# ---------------------------------------------------------------------------
# Main viewer (RHI-based). All layers go through Qt RHI; the OpenGL-only
# layers in src/layers/{Detector,EdgeColor,Network,PedArea,POI,Polygon,Rail,
# StopLine,StoppingPlace,TLS,Vehicle,Person}Layer.cpp are no longer compiled
# in. RhiHostWidget (Qt 6.4-6.6) or QRhiWidget (Qt 6.7+) hosts the GPU output;
# NetworkOverlayWidget paints QPainter overlays on top.
# ---------------------------------------------------------------------------

add_executable(qt_cpp
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/NetworkView.cpp
    src/NetworkView.h
    src/NetworkOverlayWidget.cpp
    src/NetworkOverlayWidget.h
    src/Camera.cpp
    src/Camera.h
    src/rhi_compat/rhi_compat.h
    src/rhi_compat/RhiWidgetBase.h
    src/rhi_compat/RhiHostWidget.cpp
    src/rhi_compat/RhiHostWidget.h
    src/rhi_compat/RhiLayerCommon.h
    src/rhi_compat/StaticTrisRhi.h
    src/rhi_compat/TrisPasses.h
    src/layers/LayerBuilders.cpp
    src/layers/LayerBuilders.h
    src/layers/POILayerRhi.cpp
    src/layers/POILayerRhi.h
    src/layers/TLSLayerRhi.cpp
    src/layers/TLSLayerRhi.h
    src/layers/VehicleLayerRhi.cpp
    src/layers/VehicleLayerRhi.h
    src/layers/PersonLayerRhi.cpp
    src/layers/PersonLayerRhi.h
    src/sim/LogCapture.cpp
    src/sim/LogCapture.h
    src/sim/NetworkGeometry.cpp
    src/sim/NetworkGeometry.h
    src/sim/NetworkCache.cpp
    src/sim/NetworkCache.h
    src/sim/SimSnapshot.h
    src/sim/SimWorker.cpp
    src/sim/SimWorker.h
)

qt6_add_shaders(qt_cpp "qt_cpp_shaders"
    PREFIX "/shaders"
    BASE "src/shaders"
    FILES
        src/shaders/vehicle.vert
        src/shaders/vehicle.frag
        src/shaders/person.vert
        src/shaders/person.frag
        src/shaders/poi.vert
        src/shaders/poi.frag
        src/shaders/tls.vert
        src/shaders/tls.frag
        src/shaders/tris_color.vert
        src/shaders/tris_color.frag
)

target_include_directories(qt_cpp PRIVATE src)
# SUMO's <utils/iodevices/OutputDevice.h> #includes <config.h> from the SUMO
# build tree.  Pull that directory in so LogCapture.cpp can subclass
# OutputDevice without needing a dummy config.h.
if (NOT WIN32)
    if(EXISTS "$ENV{SUMO_HOME}/cmclaude/src/config.h")
        target_include_directories(qt_cpp PRIVATE "$ENV{SUMO_HOME}/cmclaude/src")
    elseif(EXISTS "$ENV{SUMO_HOME}/build/src/config.h")
        target_include_directories(qt_cpp PRIVATE "$ENV{SUMO_HOME}/build/src")
    elseif(EXISTS "$ENV{SUMO_HOME}/cmvs/Release/src/config.h")
        target_include_directories(qt_cpp PRIVATE "$ENV{SUMO_HOME}/cmvs/Release/src")
    endif()
endif()
target_link_libraries(qt_cpp PRIVATE
    Qt6::Widgets
    Qt6::WidgetsPrivate
    Qt6::GuiPrivate
    SUMO::libsumocpp
    protobuf::libprotobuf
)

set_target_properties(qt_cpp PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${LIBSUMO_LIBRARY_DIR}"
)
