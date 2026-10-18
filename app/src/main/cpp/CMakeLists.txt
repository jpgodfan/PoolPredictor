cmake_minimum_required(3.22.1)

project("poolpredictor")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_library(
        poolpredictor
        SHARED
        predictor.cpp
)

target_include_directories(
        poolpredictor
        PRIVATE
        ${OpenCV_INCLUDE_DIRS}
)

find_library(log-lib log)

target_link_libraries(
        poolpredictor
        ${OpenCV_LIBS}
        ${log-lib}
)
