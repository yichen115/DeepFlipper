# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# The full board dependency graph can exceed CreateProcess's command-line limit
# on Windows, especially in a deep checkout. Let Ninja pass arguments by file.
if(CMAKE_HOST_WIN32)
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
endif()

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.3")
project(deepflipper)
