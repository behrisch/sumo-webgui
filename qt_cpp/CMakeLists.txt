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

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets ShaderTools)
find_package(Libsumo REQUIRED)
# Used to read the shared `.pb` network cache the ecal_deck publisher
# writes; the lite runtime is sufficient since we only deserialize
# typed-array `bytes` payloads + a few scalars. See "Shared `.pb` network
# cache + early-render" in PLAN.md for the design.
find_package(Protobuf REQUIRED)

# Generate sumo.pb.{cc,h} from the shared proto so qt_cpp can deserialize
# the network cache written by ecal_deck's Python publisher.
set(SUMO_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/../ecal_deck/proto/sumo.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")
set(SUMO_PB_CC "${PROTO_GEN_DIR}/sumo.pb.cc")
set(SUMO_PB_H  "${PROTO_GEN_DIR}/sumo.pb.h")
add_custom_command(
    OUTPUT "${SUMO_PB_CC}" "${SUMO_PB_H}"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out=${PROTO_GEN_DIR}
            -I ${CMAKE_CURRENT_SOURCE_DIR}/../ecal_deck/proto
            ${SUMO_PROTO}
    DEPENDS "${SUMO_PROTO}"
    COMMENT "Generating sumo.pb.{cc,h} from ${SUMO_PROTO}"
    VERBATIM
)

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
    ${SUMO_PB_CC}
    ${SUMO_PB_H}
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

target_include_directories(qt_cpp PRIVATE src "${PROTO_GEN_DIR}")
# SUMO's <utils/iodevices/OutputDevice.h> #includes <config.h> from the SUMO
# build tree.  Pull that directory in so LogCapture.cpp can subclass
# OutputDevice without needing a dummy config.h.
if(EXISTS "${SUMO_HOME}/cmclaude/src/config.h")
    target_include_directories(qt_cpp PRIVATE "${SUMO_HOME}/cmclaude/src")
elseif(EXISTS "${SUMO_HOME}/build/src/config.h")
    target_include_directories(qt_cpp PRIVATE "${SUMO_HOME}/build/src")
elseif(EXISTS "${SUMO_HOME}/cmake-build-release/src/config.h")
    target_include_directories(qt_cpp PRIVATE "${SUMO_HOME}/cmake-build-release/src")
endif()
target_link_libraries(qt_cpp PRIVATE
    Qt6::Widgets
    Qt6::WidgetsPrivate
    Qt6::GuiPrivate
    Libsumo::Libsumo
    protobuf::libprotobuf
)

set_target_properties(qt_cpp PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${LIBSUMO_LIBRARY_DIR}"
)

# ---------------------------------------------------------------------------
# Spike viewer (kept for incremental RHI experiments). Same RhiHostWidget,
# but only the vehicle layer wired up; lets us iterate on shaders without
# pulling in the full network-view pipeline.
# ---------------------------------------------------------------------------
add_executable(qt_cpp_rhi_spike
    src/rhi_spike/main_rhi.cpp
    src/rhi_spike/RhiSpikeWidget.cpp
    src/rhi_spike/RhiSpikeWidget.h
    src/rhi_compat/rhi_compat.h
    src/rhi_compat/RhiWidgetBase.h
    src/rhi_compat/RhiHostWidget.cpp
    src/rhi_compat/RhiHostWidget.h
    src/layers/VehicleLayerRhi.cpp
    src/layers/VehicleLayerRhi.h
    src/Camera.cpp
    src/Camera.h
    src/sim/LogCapture.cpp
    src/sim/LogCapture.h
    src/sim/NetworkGeometry.cpp
    src/sim/NetworkGeometry.h
    src/sim/SimSnapshot.h
    src/sim/SimWorker.cpp
    src/sim/SimWorker.h
)

qt6_add_shaders(qt_cpp_rhi_spike "spike_shaders"
    PREFIX "/shaders"
    BASE "src/shaders"
    FILES
        src/shaders/vehicle.vert
        src/shaders/vehicle.frag
        src/shaders/person.vert
        src/shaders/person.frag
)

target_include_directories(qt_cpp_rhi_spike PRIVATE src)
if(EXISTS "${SUMO_HOME}/cmclaude/src/config.h")
    target_include_directories(qt_cpp_rhi_spike PRIVATE "${SUMO_HOME}/cmclaude/src")
elseif(EXISTS "${SUMO_HOME}/build/src/config.h")
    target_include_directories(qt_cpp_rhi_spike PRIVATE "${SUMO_HOME}/build/src")
elseif(EXISTS "${SUMO_HOME}/cmake-build-release/src/config.h")
    target_include_directories(qt_cpp_rhi_spike PRIVATE "${SUMO_HOME}/cmake-build-release/src")
endif()
target_link_libraries(qt_cpp_rhi_spike PRIVATE
    Qt6::Widgets
    Qt6::WidgetsPrivate
    Qt6::GuiPrivate
    Libsumo::Libsumo
)

set_target_properties(qt_cpp_rhi_spike PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${LIBSUMO_LIBRARY_DIR}"
)
