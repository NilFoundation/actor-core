file(READ ${ACTOR_DPDK_CONFIG_FILE_IN} dpdk_config)
file(STRINGS ${ACTOR_DPDK_CONFIG_FILE_CHANGES} dpdk_config_changes)
set(word_pattern "[^\n\r \t]+")

foreach(var ${dpdk_config_changes})
    if(var MATCHES "(${word_pattern})=(${word_pattern})")
        set(key ${CMAKE_MATCH_1})
        set(value ${CMAKE_MATCH_2})

        string(REGEX REPLACE
               "${key}=${word_pattern}"
               "${key}=${value}"
               dpdk_config
               ${dpdk_config})
    endif()
endforeach()

file(WRITE ${ACTOR_DPDK_CONFIG_FILE_OUT} ${dpdk_config})
file(APPEND ${ACTOR_DPDK_CONFIG_FILE_OUT} "CONFIG_RTE_MACHINE=${ACTOR_DPDK_MACHINE}")
