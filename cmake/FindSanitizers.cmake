include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS -fsanitize=address)
check_cxx_source_compiles("int main() {}" Sanitizers_ADDRESS_FOUND)

if(Sanitizers_ADDRESS_FOUND)
    set(Sanitizers_ADDRESS_COMPILER_OPTIONS -fsanitize=address)
endif()

set(CMAKE_REQUIRED_FLAGS -fsanitize=undefined)
check_cxx_source_compiles("int main() {}" Sanitizers_UNDEFINED_BEHAVIOR_FOUND)

if(Sanitizers_UNDEFINED_BEHAVIOR_FOUND)
    # Disable vptr because of https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88684
    set(Sanitizers_UNDEFINED_BEHAVIOR_COMPILER_OPTIONS "-fsanitize=undefined;-fno-sanitize=vptr")
endif()

set(Sanitizers_COMPILER_OPTIONS
    ${Sanitizers_ADDRESS_COMPILER_OPTIONS}
    ${Sanitizers_UNDEFINED_BEHAVIOR_COMPILER_OPTIONS})

file(READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/Sanitizers_fiber_test.cc _sanitizers_fiber_test_code)
set(CMAKE_REQUIRED_FLAGS ${Sanitizers_COMPILER_OPTIONS})
check_cxx_source_compiles("${_sanitizers_fiber_test_code}" Sanitizers_FIBER_SUPPORT)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(Sanitizers
                                  REQUIRED_VARS
                                  Sanitizers_ADDRESS_COMPILER_OPTIONS
                                  Sanitizers_UNDEFINED_BEHAVIOR_COMPILER_OPTIONS)

if(Sanitizers_FOUND)
    if(NOT (TARGET Sanitizers::address))
        add_library(Sanitizers::address INTERFACE IMPORTED)

        set_target_properties(Sanitizers::address
                              PROPERTIES
                              INTERFACE_COMPILE_OPTIONS ${Sanitizers_ADDRESS_COMPILER_OPTIONS}
                              INTERFACE_LINK_LIBRARIES ${Sanitizers_ADDRESS_COMPILER_OPTIONS})
    endif()

    if(NOT (TARGET Sanitizers::undefined_behavior))
        add_library(Sanitizers::undefined_behavior INTERFACE IMPORTED)

        set_target_properties(Sanitizers::undefined_behavior
                              PROPERTIES
                              INTERFACE_COMPILE_OPTIONS "${Sanitizers_UNDEFINED_BEHAVIOR_COMPILER_OPTIONS}"
                              INTERFACE_LINK_LIBRARIES "${Sanitizers_UNDEFINED_BEHAVIOR_COMPILER_OPTIONS}")
    endif()
endif()
