# Support preference of static libs by adjusting CMAKE_FIND_LIBRARY_SUFFIXES
if(lz4_PC_STATIC_LIBS)
    set(_lz4_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
endif()

find_package(PkgConfig REQUIRED)

pkg_search_module(lz4_PC liblz4)

find_library(lz4_LIBRARY
             NAMES lz4
             HINTS
             ${lz4_PC_LIBDIR}
             ${lz4_PC_LIBRARY_DIRS})

find_path(lz4_INCLUDE_DIR
          NAMES lz4.h
          HINTS
          ${lz4_PC_INCLUDEDIR}
          ${lz4_PC_INCLUDEDIRS})

mark_as_advanced(
        lz4_LIBRARY
        lz4_INCLUDE_DIR)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(lz4
                                  REQUIRED_VARS
                                  lz4_LIBRARY
                                  lz4_INCLUDE_DIR
                                  VERSION_VAR lz4_PC_VERSION)

set(lz4_LIBRARIES ${lz4_LIBRARY})
set(lz4_INCLUDE_DIRS ${lz4_INCLUDE_DIR})

if(lz4_FOUND)
    set(CMAKE_REQUIRED_LIBRARIES ${lz4_LIBRARY})
    include(CheckSymbolExists)

    check_symbol_exists(LZ4_compress_default
                        ${lz4_INCLUDE_DIR}/lz4.h
                        lz4_HAVE_COMPRESS_DEFAULT)
endif()

if(lz4_FOUND AND NOT (TARGET lz4::lz4))
    add_library(lz4::lz4 UNKNOWN IMPORTED)

    set_target_properties(lz4::lz4
                          PROPERTIES
                          IMPORTED_LOCATION ${lz4_LIBRARY}
                          INTERFACE_INCLUDE_DIRECTORIES ${lz4_INCLUDE_DIRS})
endif()

# Restore the original find library ordering
if(lz4_PC_STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_lz4_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

