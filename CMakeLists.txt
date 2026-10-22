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
set(ORDER_BOOK_INVARIANT_CHECKS AUTO CACHE STRING
    "Check the whole book after every public command: AUTO (only where NDEBUG is undefined, i.e. Debug), ON, or OFF")
set_property(CACHE ORDER_BOOK_INVARIANT_CHECKS PROPERTY STRINGS AUTO ON OFF)
if(NOT ORDER_BOOK_INVARIANT_CHECKS MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ORDER_BOOK_INVARIANT_CHECKS must be AUTO, ON, or OFF, not '${ORDER_BOOK_INVARIANT_CHECKS}'")
endif()

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
    src/order_book_invariants.cpp
    src/order.cpp
    src/trade.cpp
    src/submission_results.cpp
    src/cancellation_results.cpp
    src/price_level.cpp
    src/event_format.cpp
)

target_compile_features(order_book PUBLIC cxx_std_20)
target_include_directories(order_book
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
order_book_enable_warnings(order_book)
order_book_enable_sanitizers(order_book)
# PUBLIC, so tests and other users of the library see the same setting it was built with.
# AUTO defines nothing and leaves the choice to NDEBUG (see invariants.hpp).
if(ORDER_BOOK_INVARIANT_CHECKS STREQUAL "ON")
    target_compile_definitions(order_book PUBLIC ORDER_BOOK_INVARIANT_CHECKS=1)
elseif(ORDER_BOOK_INVARIANT_CHECKS STREQUAL "OFF")
    target_compile_definitions(order_book PUBLIC ORDER_BOOK_INVARIANT_CHECKS=0)
endif()

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

    add_executable(submission_tests
        tests/submission_test.cpp
    )

    target_compile_features(submission_tests PRIVATE cxx_std_20)
    target_link_libraries(submission_tests PRIVATE order_book)
    order_book_enable_warnings(submission_tests)
    order_book_enable_sanitizers(submission_tests)
    add_test(NAME submission_tests COMMAND submission_tests)

    add_executable(event_tests
        tests/event_test.cpp
    )

    target_compile_features(event_tests PRIVATE cxx_std_20)
    target_link_libraries(event_tests PRIVATE order_book)
    order_book_enable_warnings(event_tests)
    order_book_enable_sanitizers(event_tests)
    add_test(NAME event_tests COMMAND event_tests)

    add_executable(event_format_tests
        tests/event_format_test.cpp
    )

    target_compile_features(event_format_tests PRIVATE cxx_std_20)
    target_link_libraries(event_format_tests PRIVATE order_book)
    order_book_enable_warnings(event_format_tests)
    order_book_enable_sanitizers(event_format_tests)
    add_test(NAME event_format_tests COMMAND event_format_tests)

    add_executable(allocation_failure_tests
        tests/allocation_failure_test.cpp
    )

    target_compile_features(allocation_failure_tests PRIVATE cxx_std_20)
    target_link_libraries(allocation_failure_tests PRIVATE order_book)
    order_book_enable_warnings(allocation_failure_tests)
    order_book_enable_sanitizers(allocation_failure_tests)
    add_test(NAME allocation_failure_tests COMMAND allocation_failure_tests)
    set_tests_properties(allocation_failure_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(invariant_tests
        tests/invariant_test.cpp
    )

    target_compile_features(invariant_tests PRIVATE cxx_std_20)
    target_link_libraries(invariant_tests PRIVATE order_book)
    order_book_enable_warnings(invariant_tests)
    order_book_enable_sanitizers(invariant_tests)
    add_test(NAME invariant_tests COMMAND invariant_tests)

    add_executable(scenario_tests
        tests/scenario_test.cpp
    )

    target_compile_features(scenario_tests PRIVATE cxx_std_20)
    target_link_libraries(scenario_tests PRIVATE order_book)
    order_book_enable_warnings(scenario_tests)
    order_book_enable_sanitizers(scenario_tests)
    add_test(NAME scenario_tests COMMAND scenario_tests)

    # Randomized command testing: seeded streams of submissions and cancellations, each command
    # checked against the book's invariants and a reference model. The harness is a library so
    # its own tests can drive it.
    add_library(order_book_randomized STATIC
        tests/randomized/abort_reporter.cpp
        tests/randomized/command.cpp
        tests/randomized/command_generator.cpp
        tests/randomized/command_replayer.cpp
        tests/randomized/failure_report.cpp
        tests/randomized/reference_book.cpp
        tests/randomized/shrink.cpp
    )

    target_compile_features(order_book_randomized PUBLIC cxx_std_20)
    target_include_directories(order_book_randomized PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/randomized)
    target_link_libraries(order_book_randomized PUBLIC order_book)
    order_book_enable_warnings(order_book_randomized)
    order_book_enable_sanitizers(order_book_randomized)

    add_executable(randomized_tests
        tests/randomized/randomized_main.cpp
    )

    target_compile_features(randomized_tests PRIVATE cxx_std_20)
    target_link_libraries(randomized_tests PRIVATE order_book_randomized)
    order_book_enable_warnings(randomized_tests)
    order_book_enable_sanitizers(randomized_tests)
    # The committed seeds, then every saved stream in tests/randomized/regressions/.
    add_test(NAME randomized_seeds
        COMMAND randomized_tests --out-dir ${CMAKE_CURRENT_BINARY_DIR}/randomized-failures)
    add_test(NAME randomized_regressions
        COMMAND randomized_tests --regressions ${CMAKE_CURRENT_SOURCE_DIR}/tests/randomized/regressions
                --out-dir ${CMAKE_CURRENT_BINARY_DIR}/randomized-failures)

    add_executable(randomized_harness_tests
        tests/randomized/harness_test.cpp
    )

    target_compile_features(randomized_harness_tests PRIVATE cxx_std_20)
    target_link_libraries(randomized_harness_tests PRIVATE order_book_randomized)
    order_book_enable_warnings(randomized_harness_tests)
    order_book_enable_sanitizers(randomized_harness_tests)
    add_test(NAME randomized_harness_tests COMMAND randomized_harness_tests)

    # Each run breaks the book, then runs one command, which should abort naming itself and
    # the violation. Pass/fail is decided by that output, since the program aborts either way.
    add_executable(invariant_boundary_tests
        tests/invariant_boundary_test.cpp
    )

    target_compile_features(invariant_boundary_tests PRIVATE cxx_std_20)
    target_link_libraries(invariant_boundary_tests PRIVATE order_book)
    order_book_enable_warnings(invariant_boundary_tests)
    order_book_enable_sanitizers(invariant_boundary_tests)
    foreach(command addOrder submitOrder cancelOrder consumeLevel)
        add_test(NAME invariant_boundary_${command} COMMAND invariant_boundary_tests ${command})
        set_tests_properties(invariant_boundary_${command} PROPERTIES
            PASS_REGULAR_EXPRESSION "order book invariant violated after ${command}: AGGREGATE_QUANTITY_MISMATCH side=SELL price=100 expected=5 actual=6"
            FAIL_REGULAR_EXPRESSION "FAILED"
            SKIP_REGULAR_EXPRESSION "SKIPPED")
    endforeach()
    # A command that throws is checked too, as its exception unwinds through the boundary.
    add_test(NAME invariant_boundary_cancelOrderThrows COMMAND invariant_boundary_tests cancelOrderThrows)
    set_tests_properties(invariant_boundary_cancelOrderThrows PROPERTIES
        PASS_REGULAR_EXPRESSION "order book invariant violated after cancelOrder: INVALID_INDEX_ENTRY side=BUY price=90 order=2 expected=90 actual=91"
        FAIL_REGULAR_EXPRESSION "FAILED"
        SKIP_REGULAR_EXPRESSION "SKIPPED")
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
