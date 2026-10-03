cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS
    "../../../../claw_modules"
    "../.."
    "../../../../common/http_reuse")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(exec_state_test)
