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

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGLWidgets ShaderTools)
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
    src/layers/DetectorLayer.cpp
    src/layers/DetectorLayer.h
    src/layers/EdgeColorLayer.cpp
    src/layers/EdgeColorLayer.h
    src/layers/NetworkLayer.cpp
    src/layers/NetworkLayer.h
    src/layers/PedAreaLayer.cpp
    src/layers/PedAreaLayer.h
    src/layers/PersonLayer.cpp
    src/layers/PersonLayer.h
    src/layers/POILayer.cpp
    src/layers/POILayer.h
    src/layers/PolygonLayer.cpp
    src/layers/PolygonLayer.h
    src/layers/RailLayer.cpp
    src/layers/RailLayer.h
    src/layers/StopLineLayer.cpp
    src/layers/StopLineLayer.h
    src/layers/StoppingPlaceLayer.cpp
    src/layers/StoppingPlaceLayer.h
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

# ---------------------------------------------------------------------------
# QRhi spike: standalone viewer that exercises the new VehicleLayerRhi class
# on a QRhiWidget (Vulkan / Metal / D3D12 / OpenGL via Qt RHI). Lets us
# validate the cross-API renderer in isolation while the OpenGL build above
# stays the production path. See PLAN.md "Graphics API choice — Qt RHI
# migration".
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
    src/layers/PersonLayerRhi.cpp
    src/layers/PersonLayerRhi.h
    src/Camera.cpp
    src/Camera.h
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
