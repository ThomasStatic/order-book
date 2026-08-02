cmake_minimum_required(VERSION 3.20)

project(order_book VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ORDER_BOOK_BUILD_BENCHMARKS "Build the benchmark executable" OFF)
option(ORDER_BOOK_ENABLE_ASAN "Enable AddressSanitizer for project-owned targets" OFF)
option(ORDER_BOOK_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for project-owned targets" OFF)
option(ORDER_BOOK_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

function(order_book_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    endif()

    if(ORDER_BOOK_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /WX)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(order_book_enable_sanitizers target_name)
    if(NOT ORDER_BOOK_ENABLE_ASAN AND NOT ORDER_BOOK_ENABLE_UBSAN)
        return()
    endif()

    if(MSVC)
        message(WARNING "Sanitizers are not configured for MSVC in this project.")
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(ORDER_BOOK_ENABLE_ASAN AND ORDER_BOOK_ENABLE_UBSAN)
            target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
        elseif(ORDER_BOOK_ENABLE_ASAN)
            target_compile_options(${target_name} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE -fsanitize=address)
        elseif(ORDER_BOOK_ENABLE_UBSAN)
            target_compile_options(${target_name} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE -fsanitize=undefined)
        endif()
    else()
        message(WARNING "Sanitizers were requested, but the compiler '${CMAKE_CXX_COMPILER_ID}' is not supported by this CMake setup.")
    endif()
endfunction()

add_library(order_book STATIC
    src/order_book.cpp
    src/order.cpp
    src/trade.cpp
    src/submission_results.cpp
)

target_compile_features(order_book PUBLIC cxx_std_20)
target_include_directories(order_book
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
order_book_enable_warnings(order_book)
order_book_enable_sanitizers(order_book)

add_executable(order_book_cli
    apps/order_book_cli.cpp
)

target_compile_features(order_book_cli PRIVATE cxx_std_20)
target_link_libraries(order_book_cli PRIVATE order_book)
order_book_enable_warnings(order_book_cli)
order_book_enable_sanitizers(order_book_cli)

include(CTest)

if(BUILD_TESTING)
    add_executable(order_book_tests
        tests/order_book_test.cpp
    )

    target_compile_features(order_book_tests PRIVATE cxx_std_20)
    target_link_libraries(order_book_tests PRIVATE order_book)
    order_book_enable_warnings(order_book_tests)
    order_book_enable_sanitizers(order_book_tests)
    add_test(NAME order_book_smoke COMMAND order_book_tests)

    add_executable(order_tests
        tests/order_test.cpp
    )

    target_compile_features(order_tests PRIVATE cxx_std_20)
    target_link_libraries(order_tests PRIVATE order_book)
    order_book_enable_warnings(order_tests)
    order_book_enable_sanitizers(order_tests)
    add_test(NAME order_tests COMMAND order_tests)
endif()

if(ORDER_BOOK_BUILD_BENCHMARKS)
    add_executable(order_book_benchmarks
        benchmarks/order_book_benchmark.cpp
    )

    target_compile_features(order_book_benchmarks PRIVATE cxx_std_20)
    target_link_libraries(order_book_benchmarks PRIVATE order_book)
    order_book_enable_warnings(order_book_benchmarks)
    order_book_enable_sanitizers(order_book_benchmarks)
endif()
