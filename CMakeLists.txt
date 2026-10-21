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

if(MSVC AND ORDER_BOOK_ENABLE_ASAN)
    # /RTC1 (in CMake's default MSVC Debug flags) cannot be combined with /fsanitize=address,
    # and the linker ignores /INCREMENTAL for ASan objects (LNK4300), so turn both off.
    string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    foreach(config DEBUG RELWITHDEBINFO)
        string(REGEX REPLACE "/INCREMENTAL(:YES|:NO)?" "" linker_flags "${CMAKE_EXE_LINKER_FLAGS_${config}}")
        set(CMAKE_EXE_LINKER_FLAGS_${config} "${linker_flags} /INCREMENTAL:NO")
    endforeach()
endif()

if(MSVC AND ORDER_BOOK_ENABLE_UBSAN)
    message(WARNING "UndefinedBehaviorSanitizer is not available with MSVC; ORDER_BOOK_ENABLE_UBSAN is ignored.")
endif()

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
        if(ORDER_BOOK_ENABLE_ASAN)
            # /Zi keeps ASan reports symbolized in every configuration.
            target_compile_options(${target_name} PRIVATE /fsanitize=address /Zi)

            # Executables load the ASan runtime DLL that ships beside cl.exe; copy it next
            # to them so they also run outside a Visual Studio developer prompt.
            get_target_property(target_type ${target_name} TYPE)
            if(target_type STREQUAL "EXECUTABLE")
                get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
                if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                    set(asan_runtime "${compiler_dir}/clang_rt.asan_dynamic-x86_64.dll")
                else()
                    set(asan_runtime "${compiler_dir}/clang_rt.asan_dynamic-i386.dll")
                endif()

                if(EXISTS "${asan_runtime}")
                    add_custom_command(TARGET ${target_name} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${asan_runtime}" "$<TARGET_FILE_DIR:${target_name}>")
                else()
                    message(WARNING "ASan runtime not found at ${asan_runtime}; put its directory on PATH to run ${target_name}.")
                endif()
            endif()
        endif()
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
    src/cancellation_results.cpp
    src/price_level.cpp
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

    add_executable(result_type_tests
        tests/result_types_test.cpp
    )

    target_compile_features(result_type_tests PRIVATE cxx_std_20)
    target_link_libraries(result_type_tests PRIVATE order_book)
    order_book_enable_warnings(result_type_tests)
    order_book_enable_sanitizers(result_type_tests)
    add_test(NAME result_type_tests COMMAND result_type_tests)

    add_executable(price_level_tests
        tests/price_level_test.cpp
    )

    target_compile_features(price_level_tests PRIVATE cxx_std_20)
    target_link_libraries(price_level_tests PRIVATE order_book)
    order_book_enable_warnings(price_level_tests)
    order_book_enable_sanitizers(price_level_tests)
    add_test(NAME price_level_tests COMMAND price_level_tests)

    add_executable(execution_tests
        tests/execution_test.cpp
    )

    target_compile_features(execution_tests PRIVATE cxx_std_20)
    target_link_libraries(execution_tests PRIVATE order_book)
    order_book_enable_warnings(execution_tests)
    order_book_enable_sanitizers(execution_tests)
    add_test(NAME execution_tests COMMAND execution_tests)

    add_executable(matching_tests
        tests/matching_test.cpp
    )

    target_compile_features(matching_tests PRIVATE cxx_std_20)
    target_link_libraries(matching_tests PRIVATE order_book)
    order_book_enable_warnings(matching_tests)
    order_book_enable_sanitizers(matching_tests)
    add_test(NAME matching_tests COMMAND matching_tests)

    add_executable(cancellation_tests
        tests/cancellation_test.cpp
    )

    target_compile_features(cancellation_tests PRIVATE cxx_std_20)
    target_link_libraries(cancellation_tests PRIVATE order_book)
    order_book_enable_warnings(cancellation_tests)
    order_book_enable_sanitizers(cancellation_tests)
    add_test(NAME cancellation_tests COMMAND cancellation_tests)

    add_executable(allocation_failure_tests
        tests/allocation_failure_test.cpp
    )

    target_compile_features(allocation_failure_tests PRIVATE cxx_std_20)
    target_link_libraries(allocation_failure_tests PRIVATE order_book)
    order_book_enable_warnings(allocation_failure_tests)
    order_book_enable_sanitizers(allocation_failure_tests)
    add_test(NAME allocation_failure_tests COMMAND allocation_failure_tests)
    set_tests_properties(allocation_failure_tests PROPERTIES SKIP_RETURN_CODE 77)
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
