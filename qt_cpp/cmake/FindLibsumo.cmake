# FindLibsumo.cmake
#
# Locates the libsumo C++ library (libsumocpp.so) and headers.
# Honors SUMO_HOME (env var or CMake variable) pointing at the SUMO source
# root. Looks under common build directory names for the .so produced by the
# `libsumocpp` CMake target.
#
# Defines:
#   Libsumo::Libsumo  IMPORTED target
#   LIBSUMO_LIBRARY   path to libsumocpp.so
#   LIBSUMO_LIBRARY_DIR  directory containing libsumocpp.so
#   LIBSUMO_INCLUDE_DIR  directory containing libsumo/Simulation.h

if(NOT SUMO_HOME)
    set(SUMO_HOME "$ENV{SUMO_HOME}")
endif()

if(NOT SUMO_HOME)
    message(FATAL_ERROR
        "SUMO_HOME is not set. Pass -DSUMO_HOME=/path/to/sumo or export the "
        "environment variable. Expected to find src/libsumo/Simulation.h "
        "under it.")
endif()

find_path(LIBSUMO_INCLUDE_DIR
    NAMES libsumo/Simulation.h
    PATHS "${SUMO_HOME}/src"
    NO_DEFAULT_PATH
)

find_library(LIBSUMO_LIBRARY
    NAMES sumocpp libsumocpp
    PATHS
        "${SUMO_HOME}/cmclaude/src/fmi/sumo-fmi2/binaries/linux64"
        "${SUMO_HOME}/build/src/fmi/sumo-fmi2/binaries/linux64"
        "${SUMO_HOME}/cmake-build/src/fmi/sumo-fmi2/binaries/linux64"
        "${SUMO_HOME}/cmclaude/src/libsumo"
        "${SUMO_HOME}/build/src/libsumo"
        "${SUMO_HOME}/lib"
        /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    NO_DEFAULT_PATH
)
if(NOT LIBSUMO_LIBRARY)
    find_library(LIBSUMO_LIBRARY NAMES sumocpp libsumocpp)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libsumo
    REQUIRED_VARS LIBSUMO_LIBRARY LIBSUMO_INCLUDE_DIR
)

if(LIBSUMO_FOUND AND NOT TARGET Libsumo::Libsumo)
    get_filename_component(LIBSUMO_LIBRARY_DIR "${LIBSUMO_LIBRARY}" DIRECTORY)
    add_library(Libsumo::Libsumo SHARED IMPORTED)
    set_target_properties(Libsumo::Libsumo PROPERTIES
        IMPORTED_LOCATION "${LIBSUMO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBSUMO_INCLUDE_DIR}"
    )
endif()
