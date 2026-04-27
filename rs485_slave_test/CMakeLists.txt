cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ../components/comm/parser
    ../components/comm/protocol
    ../components/hal/rs485
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(rs485_slave_test)
