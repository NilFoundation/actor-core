# Support preference of static libs by adjusting CMAKE_FIND_LIBRARY_SUFFIXES
if(c-ares_PC_STATIC_LIBS)
    set(_c_ares_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(c-ares_PC libcares)

find_library(c-ares_LIBRARY
             NAMES cares
             HINTS
             ${c-ares_PC_LIBDIR}
             ${c-ares_PC_LIBRARY_DIRS})

find_path(c-ares_INCLUDE_DIR
          NAMES ares_dns.h
          HINTS
          ${c-ares_PC_INCLUDEDIR}
          ${c-ares_PC_INCLUDE_DIRS})

mark_as_advanced(
        c-ares_LIBRARY
        c-ares_INCLUDE_DIR)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(c-ares
                                  REQUIRED_VARS
                                  c-ares_LIBRARY
                                  c-ares_INCLUDE_DIR
                                  VERSION_VAR c-ares_PC_VERSION)

set(c-ares_LIBRARIES ${c-ares_LIBRARY})
set(c-ares_INCLUDE_DIRS ${c-ares_INCLUDE_DIR})

if(c-ares_FOUND AND NOT (TARGET c-ares::c-ares))
    add_library(c-ares::c-ares UNKNOWN IMPORTED)

    set_target_properties(c-ares::c-ares
                          PROPERTIES
                          IMPORTED_LOCATION ${c-ares_LIBRARY}
                          INTERFACE_INCLUDE_DIRECTORIES ${c-ares_INCLUDE_DIRS})
endif()

# Restore the original find library ordering
if(c-ares_PC_STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_c_ares_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

