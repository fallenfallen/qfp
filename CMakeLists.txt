project(fp)

cmake_minimum_required(VERSION 2.6.0)
find_package(Qt4 COMPONENTS QtCore REQUIRED)
include(${QT_USE_FILE})

set(fp_SRCS
    test/commandlinethread.h
    test/errorparser.cpp
    test/main.cpp
    )


include_directories(qfp/3partys/qextserialport/src)
include_directories(qfp/src)
add_subdirectory(qfp)

add_executable(fp ${fp_SRCS})
set_target_properties(fp PROPERTIES AUTOMOC true)
target_link_libraries(fp ${QT_LIBRARIES})
target_link_libraries(fp qfp)

