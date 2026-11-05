cmake_minimum_required(VERSION 3.22)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(EXTRA_COMPONENT_DIRS ${ROOT}/shared/components/text ${ROOT}/shared/components/ble_setup
                         ${ROOT}/components/oled ${ROOT}/components/scan_core)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(site_scanner)
