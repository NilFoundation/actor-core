# Support preference of static libs by adjusting CMAKE_FIND_LIBRARY_SUFFIXES
if(yaml-cpp_PC_STATIC_LIBS)
    set(_yaml_cpp_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
endif()

find_package(PkgConfig REQUIRED)

pkg_search_module(yaml-cpp_PC yaml-cpp)

find_library(yaml-cpp_LIBRARY
             NAMES yaml-cpp
             HINTS
             ${yaml-cpp_PC_LIBDIR}
             ${yaml-cpp_PC_LIBRARY_DIRS})

find_path(yaml-cpp_INCLUDE_DIR
          NAMES yaml-cpp/yaml.h
          PATH_SUFFIXES yaml-cpp
          HINTS
          ${yaml-cpp_PC_INCLUDEDIR}
          ${yaml-cpp_PC_INCLUDE_DIRS})

mark_as_advanced(
        yaml-cpp_LIBRARY
        yaml-cpp_INCLUDE_DIR)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(yaml-cpp
                                  REQUIRED_VARS
                                  yaml-cpp_LIBRARY
                                  yaml-cpp_INCLUDE_DIR
                                  VERSION_VAR yaml-cpp_PC_VERSION)

set(yaml-cpp_LIBRARIES ${yaml-cpp_LIBRARY})
set(yaml-cpp_INCLUDE_DIRS ${yaml-cpp_INCLUDE_DIR})

if(yaml-cpp_FOUND AND NOT (TARGET yaml-cpp::yaml-cpp))
    add_library(yaml-cpp::yaml-cpp UNKNOWN IMPORTED)

    set_target_properties(yaml-cpp::yaml-cpp
                          PROPERTIES
                          IMPORTED_LOCATION ${yaml-cpp_LIBRARY}
                          INTERFACE_INCLUDE_DIRECTORIES ${yaml-cpp_INCLUDE_DIRS})
endif()

# Restore the original find library ordering
if(yaml-cpp_PC_STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_yaml_cpp_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

