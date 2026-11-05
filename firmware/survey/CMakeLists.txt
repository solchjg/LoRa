cmake_minimum_required(VERSION 3.22)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(EXTRA_COMPONENT_DIRS ${ROOT}/shared/components/text ${ROOT}/components/oled)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lora_survey)
