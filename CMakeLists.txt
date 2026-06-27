cmake_minimum_required(VERSION 3.20)

project(order_book VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(order_book STATIC
    src/order_book.cpp
)

target_include_directories(order_book
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(order_book PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(order_book_tests
        tests/order_book_test.cpp
    )

    target_link_libraries(order_book_tests
        PRIVATE
            order_book
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(order_book_tests)
endif()
