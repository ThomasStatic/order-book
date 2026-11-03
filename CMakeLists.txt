cmake_minimum_required(VERSION 3.20)

project(order_book VERSION 2.0.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ORDER_BOOK_BUILD_BENCHMARKS "Build the benchmark executable" OFF)
option(ORDER_BOOK_BUILD_PROFILING "Build the profiling tools in benchmarks/profiling (needs ORDER_BOOK_BUILD_BENCHMARKS)" OFF)
option(ORDER_BOOK_ENABLE_ASAN "Enable AddressSanitizer for project-owned targets" OFF)
option(ORDER_BOOK_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for project-owned targets" OFF)
option(ORDER_BOOK_ENABLE_TSAN "Enable ThreadSanitizer for project-owned targets" OFF)
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

if(ORDER_BOOK_ENABLE_TSAN AND ORDER_BOOK_ENABLE_ASAN)
    message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer cannot be combined: build them separately.")
endif()
if(ORDER_BOOK_ENABLE_TSAN AND MSVC)
    message(FATAL_ERROR "ThreadSanitizer is not available with MSVC.")
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
    if(NOT ORDER_BOOK_ENABLE_ASAN AND NOT ORDER_BOOK_ENABLE_UBSAN AND NOT ORDER_BOOK_ENABLE_TSAN)
        return()
    endif()

    if(MSVC)
        if(ORDER_BOOK_ENABLE_ASAN)
            # /Zi (with /DEBUG below) keeps ASan reports symbolized in every configuration.
            target_compile_options(${target_name} PRIVATE /fsanitize=address /Zi)

            # Executables load the ASan runtime DLL that ships beside cl.exe; copy it next
            # to them so they also run outside a Visual Studio developer prompt.
            get_target_property(target_type ${target_name} TYPE)
            if(target_type STREQUAL "EXECUTABLE")
                # Release links without /DEBUG by default, which drops the /Zi symbols (LNK4302).
                target_link_options(${target_name} PRIVATE /DEBUG)

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
        set(sanitizers)
        if(ORDER_BOOK_ENABLE_ASAN)
            list(APPEND sanitizers address)
        endif()
        if(ORDER_BOOK_ENABLE_UBSAN)
            list(APPEND sanitizers undefined)
        endif()
        if(ORDER_BOOK_ENABLE_TSAN)
            list(APPEND sanitizers thread)
        endif()
        list(JOIN sanitizers "," sanitizer_list)

        target_compile_options(${target_name} PRIVATE -fsanitize=${sanitizer_list} -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=${sanitizer_list})
        if(ORDER_BOOK_ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Clang's static runtime defines operator new, which collides with the tests that
            # replace it to count allocations. The shared one lets their replacements win, and
            # nothing else tells a program where it lives.
            execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                            OUTPUT_VARIABLE clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
            target_link_options(${target_name} PRIVATE -shared-libsan "-Wl,-rpath,${clang_resource_dir}/lib/linux")
        endif()
        if(ORDER_BOOK_ENABLE_UBSAN)
            # UBSan otherwise prints a report and carries on, so the test would still pass.
            target_compile_options(${target_name} PRIVATE -fno-sanitize-recover=undefined)
            target_link_options(${target_name} PRIVATE -fno-sanitize-recover=undefined)
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
    src/submission_result.cpp
    src/result_recorder.cpp
    src/replace_result.cpp
    src/cancellation_result.cpp
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

# The CSV replay parser and replay engine (docs/replay-format.md). A library of its own, so
# order_book itself knows nothing about CSV or replays.
add_library(order_book_replay STATIC
    src/command.cpp
    src/command_log.cpp
    src/replay_parser.cpp
    src/replay_engine.cpp
    src/replay_inputs.cpp
    src/replay_instruments.cpp
    src/replay_summary.cpp
)

target_compile_features(order_book_replay PUBLIC cxx_std_20)
target_link_libraries(order_book_replay PUBLIC order_book)
order_book_enable_warnings(order_book_replay)
order_book_enable_sanitizers(order_book_replay)

# The engine's single-threaded pieces (docs/internals/runtime.md). Not public API.
find_package(Threads REQUIRED)
add_library(order_book_runtime STATIC
    src/runtime/abort_report.cpp
    src/runtime/book_set.cpp
    src/runtime/engine_settings.cpp
    src/runtime/instruments.cpp
    src/runtime/matching_loop.cpp
    src/runtime/shard.cpp
    src/runtime/thread_placement.cpp
)

target_compile_features(order_book_runtime PUBLIC cxx_std_20)
target_link_libraries(order_book_runtime PUBLIC order_book_replay Threads::Threads)
order_book_enable_warnings(order_book_runtime)
order_book_enable_sanitizers(order_book_runtime)

# The wire protocol's pure half (docs/wire-protocol.md). Not public API.
add_library(order_book_network STATIC
    src/network/client_stream.cpp
    src/network/engine.cpp
    src/network/entry_check.cpp
    src/network/gateway_thread.cpp
    src/network/in_memory_socket_set.cpp
    src/network/poll_socket_set.cpp
    src/network/stop_source.cpp
    src/network/tcp.cpp
    src/network/wire_codec.cpp
)
if(WIN32)
    target_link_libraries(order_book_network PRIVATE ws2_32)
endif()

target_compile_features(order_book_network PUBLIC cxx_std_20)
target_link_libraries(order_book_network PUBLIC order_book_runtime)
order_book_enable_warnings(order_book_network)
order_book_enable_sanitizers(order_book_network)

add_executable(order_book_cli
    apps/order_book_cli.cpp
)

# The engine as a process (docs/internals/network.md): only parsing and the exit status are here.
add_executable(order_book_engine
    apps/order_book_engine.cpp
    apps/engine_handlers.cpp
)

target_compile_features(order_book_engine PRIVATE cxx_std_20)
target_link_libraries(order_book_engine PRIVATE order_book_network)
order_book_enable_warnings(order_book_engine)
order_book_enable_sanitizers(order_book_engine)

target_compile_features(order_book_cli PRIVATE cxx_std_20)
target_link_libraries(order_book_cli PRIVATE order_book_network)
order_book_enable_warnings(order_book_cli)
order_book_enable_sanitizers(order_book_cli)

include(CTest)

# The independent model of the book (tests/reference/), shared by the randomized harness and
# the behavior benchmark workloads; it never touches an OrderBook.
if(BUILD_TESTING OR ORDER_BOOK_BUILD_BENCHMARKS)
    add_library(order_book_reference STATIC
        tests/reference/interleaver.cpp
        tests/reference/reference_book.cpp
    )
    target_compile_features(order_book_reference PUBLIC cxx_std_20)
    target_include_directories(order_book_reference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference)
    target_link_libraries(order_book_reference PUBLIC order_book)
    order_book_enable_warnings(order_book_reference)
    order_book_enable_sanitizers(order_book_reference)

    # A real child process, with a limit on every wait that kills it. The engine's process tests
    # and the engine benchmark kind and round-trip interval share it.
    find_package(Threads REQUIRED)
    add_library(order_book_process_testing STATIC
        tests/process/child_process.cpp
    )

    target_compile_features(order_book_process_testing PUBLIC cxx_std_20)
    target_include_directories(order_book_process_testing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/process)
    target_link_libraries(order_book_process_testing PUBLIC Threads::Threads)
    order_book_enable_warnings(order_book_process_testing)
    order_book_enable_sanitizers(order_book_process_testing)
endif()

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

    add_executable(order_behavior_tests
        tests/order_behavior_test.cpp
    )

    target_compile_features(order_behavior_tests PRIVATE cxx_std_20)
    target_link_libraries(order_behavior_tests PRIVATE order_book)
    order_book_enable_warnings(order_behavior_tests)
    order_book_enable_sanitizers(order_behavior_tests)
    add_test(NAME order_behavior_tests COMMAND order_behavior_tests)

    add_executable(result_type_tests
        tests/result_types_test.cpp
    )

    target_compile_features(result_type_tests PRIVATE cxx_std_20)
    target_link_libraries(result_type_tests PRIVATE order_book)
    order_book_enable_warnings(result_type_tests)
    order_book_enable_sanitizers(result_type_tests)
    add_test(NAME result_type_tests COMMAND result_type_tests)

    add_executable(small_vector_tests
        tests/small_vector_test.cpp
    )

    target_compile_features(small_vector_tests PRIVATE cxx_std_20)
    target_link_libraries(small_vector_tests PRIVATE order_book)
    order_book_enable_warnings(small_vector_tests)
    order_book_enable_sanitizers(small_vector_tests)
    add_test(NAME small_vector_tests COMMAND small_vector_tests)

    add_executable(node_pool_tests
        tests/node_pool_test.cpp
    )

    target_compile_features(node_pool_tests PRIVATE cxx_std_20)
    target_link_libraries(node_pool_tests PRIVATE order_book)
    order_book_enable_warnings(node_pool_tests)
    order_book_enable_sanitizers(node_pool_tests)
    add_test(NAME node_pool_tests COMMAND node_pool_tests)

    # Under ASan this dies on a read of a freed block, so it passes on the report; elsewhere
    # it has nothing to test and reports Skipped.
    add_executable(node_pool_poison_tests
        tests/node_pool_poison_test.cpp
    )

    target_compile_features(node_pool_poison_tests PRIVATE cxx_std_20)
    target_link_libraries(node_pool_poison_tests PRIVATE order_book)
    order_book_enable_warnings(node_pool_poison_tests)
    order_book_enable_sanitizers(node_pool_poison_tests)
    add_test(NAME node_pool_poison_tests COMMAND node_pool_poison_tests)
    set_tests_properties(node_pool_poison_tests PROPERTIES
        PASS_REGULAR_EXPRESSION "use-after-poison"
        FAIL_REGULAR_EXPRESSION "FAILED"
        SKIP_REGULAR_EXPRESSION "SKIPPED")

    add_executable(order_index_tests
        tests/order_index_test.cpp
    )

    target_compile_features(order_index_tests PRIVATE cxx_std_20)
    target_link_libraries(order_index_tests PRIVATE order_book)
    order_book_enable_warnings(order_index_tests)
    order_book_enable_sanitizers(order_index_tests)
    add_test(NAME order_index_tests COMMAND order_index_tests)

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

    add_executable(result_reuse_tests
        tests/result_reuse_test.cpp
    )

    target_compile_features(result_reuse_tests PRIVATE cxx_std_20)
    target_link_libraries(result_reuse_tests PRIVATE order_book)
    order_book_enable_warnings(result_reuse_tests)
    order_book_enable_sanitizers(result_reuse_tests)
    add_test(NAME result_reuse_tests COMMAND result_reuse_tests)

    add_executable(capacity_tests
        tests/capacity_test.cpp
    )

    target_compile_features(capacity_tests PRIVATE cxx_std_20)
    target_link_libraries(capacity_tests PRIVATE order_book)
    order_book_enable_warnings(capacity_tests)
    order_book_enable_sanitizers(capacity_tests)
    add_test(NAME capacity_tests COMMAND capacity_tests)

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

    add_executable(replay_parser_tests
        tests/replay_parser_test.cpp
    )

    target_compile_features(replay_parser_tests PRIVATE cxx_std_20)
    target_link_libraries(replay_parser_tests PRIVATE order_book_replay)
    order_book_enable_warnings(replay_parser_tests)
    order_book_enable_sanitizers(replay_parser_tests)
    # Also reads every sample file in tests/replay/.
    add_test(NAME replay_parser_tests COMMAND replay_parser_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay)

    add_executable(command_log_tests
        tests/command_log_test.cpp
    )

    target_compile_features(command_log_tests PRIVATE cxx_std_20)
    target_link_libraries(command_log_tests PRIVATE order_book_replay)
    order_book_enable_warnings(command_log_tests)
    order_book_enable_sanitizers(command_log_tests)
    add_test(NAME command_log_tests COMMAND command_log_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay)

    add_executable(symbol_tests
        tests/symbol_test.cpp
    )

    target_compile_features(symbol_tests PRIVATE cxx_std_20)
    target_link_libraries(symbol_tests PRIVATE order_book_replay)
    order_book_enable_warnings(symbol_tests)
    order_book_enable_sanitizers(symbol_tests)
    add_test(NAME symbol_tests COMMAND symbol_tests)

    add_executable(replay_instruments_tests
        tests/replay_instruments_test.cpp
    )

    target_compile_features(replay_instruments_tests PRIVATE cxx_std_20)
    target_link_libraries(replay_instruments_tests PRIVATE order_book_replay)
    order_book_enable_warnings(replay_instruments_tests)
    order_book_enable_sanitizers(replay_instruments_tests)
    add_test(NAME replay_instruments_tests COMMAND replay_instruments_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay)

    add_executable(instrument_directory_tests
        tests/instrument_directory_test.cpp
    )

    target_compile_features(instrument_directory_tests PRIVATE cxx_std_20)
    target_link_libraries(instrument_directory_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(instrument_directory_tests)
    order_book_enable_sanitizers(instrument_directory_tests)
    add_test(NAME instrument_directory_tests COMMAND instrument_directory_tests)

    add_executable(book_set_tests
        tests/book_set_test.cpp
    )

    target_compile_features(book_set_tests PRIVATE cxx_std_20)
    target_link_libraries(book_set_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(book_set_tests)
    order_book_enable_sanitizers(book_set_tests)
    add_test(NAME book_set_tests COMMAND book_set_tests)

    # Threaded tests use a fixed command count, never a time limit, and take turns, so one test's
    # timing isn't another's noise. ThreadSanitizer and Arm CI run exactly this label.
    add_executable(ring_tests
        tests/ring_test.cpp
    )

    target_compile_features(ring_tests PRIVATE cxx_std_20)
    target_link_libraries(ring_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(ring_tests)
    order_book_enable_sanitizers(ring_tests)
    add_test(NAME ring_tests COMMAND ring_tests)
    set_tests_properties(ring_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads)

    add_executable(matching_loop_tests
        tests/matching_loop_test.cpp
    )

    target_compile_features(matching_loop_tests PRIVATE cxx_std_20)
    target_link_libraries(matching_loop_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(matching_loop_tests)
    order_book_enable_sanitizers(matching_loop_tests)
    add_test(NAME matching_loop_tests COMMAND matching_loop_tests)
    # The timeout is what fails a lost wake-up, which hangs.
    set_tests_properties(matching_loop_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)

    add_executable(matching_abort_tests
        tests/matching_abort_test.cpp
    )

    target_compile_features(matching_abort_tests PRIVATE cxx_std_20)
    target_link_libraries(matching_abort_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(matching_abort_tests)
    order_book_enable_sanitizers(matching_abort_tests)
    add_test(NAME matching_abort_tests COMMAND matching_abort_tests)
    set_tests_properties(matching_abort_tests PROPERTIES
        PASS_REGULAR_EXPRESSION "order book runtime: shard 0 symbol AAPL +position [0-9]+ command ADD id=[0-9]+: "
        FAIL_REGULAR_EXPRESSION "FAILED"
        SKIP_REGULAR_EXPRESSION "SKIPPED")

    # A deliberate race that must be reported, so a passing ThreadSanitizer job means something.
    if(ORDER_BOOK_ENABLE_TSAN)
        add_executable(tsan_control_tests
            tests/tsan_control_test.cpp
        )

        target_compile_features(tsan_control_tests PRIVATE cxx_std_20)
        target_link_libraries(tsan_control_tests PRIVATE Threads::Threads)
        order_book_enable_warnings(tsan_control_tests)
        order_book_enable_sanitizers(tsan_control_tests)
        add_test(NAME tsan_control_tests
            COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:tsan_control_tests>
                    "-DOUTPUT_REGEX=ThreadSanitizer: data race" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_failure.cmake)
        set_tests_properties(tsan_control_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads)
    endif()

    add_executable(shard_tests
        tests/shard_test.cpp
    )

    target_compile_features(shard_tests PRIVATE cxx_std_20)
    target_link_libraries(shard_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(shard_tests)
    order_book_enable_sanitizers(shard_tests)
    add_test(NAME shard_tests COMMAND shard_tests)

    add_executable(shard_invariant_tests
        tests/shard_invariant_test.cpp
    )

    target_compile_features(shard_invariant_tests PRIVATE cxx_std_20)
    target_link_libraries(shard_invariant_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(shard_invariant_tests)
    order_book_enable_sanitizers(shard_invariant_tests)
    add_test(NAME shard_invariant_tests COMMAND shard_invariant_tests)

    add_executable(book_set_thread_tests
        tests/book_set_thread_test.cpp
    )

    target_compile_features(book_set_thread_tests PRIVATE cxx_std_20)
    target_link_libraries(book_set_thread_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(book_set_thread_tests)
    order_book_enable_sanitizers(book_set_thread_tests)
    foreach(call execute book)
        add_test(NAME book_set_thread_${call} COMMAND book_set_thread_tests ${call})
        set_tests_properties(book_set_thread_${call} PROPERTIES
            PASS_REGULAR_EXPRESSION "order book runtime: BookSet::${call} called off its owning thread: shard=0 slot=0"
            FAIL_REGULAR_EXPRESSION "FAILED"
            SKIP_REGULAR_EXPRESSION "SKIPPED")
    endforeach()

    add_executable(wire_codec_tests
        tests/wire_codec_test.cpp
    )

    target_compile_features(wire_codec_tests PRIVATE cxx_std_20)
    target_link_libraries(wire_codec_tests PRIVATE order_book_network)
    order_book_enable_warnings(wire_codec_tests)
    order_book_enable_sanitizers(wire_codec_tests)
    add_test(NAME wire_codec_tests COMMAND wire_codec_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/wire)

    add_executable(client_stream_tests
        tests/client_stream_test.cpp
    )

    target_compile_features(client_stream_tests PRIVATE cxx_std_20)
    target_link_libraries(client_stream_tests PRIVATE order_book_network)
    order_book_enable_warnings(client_stream_tests)
    order_book_enable_sanitizers(client_stream_tests)
    add_test(NAME client_stream_tests COMMAND client_stream_tests)

    add_executable(entry_check_tests
        tests/entry_check_test.cpp
    )

    target_compile_features(entry_check_tests PRIVATE cxx_std_20)
    target_link_libraries(entry_check_tests PRIVATE order_book_network)
    order_book_enable_warnings(entry_check_tests)
    order_book_enable_sanitizers(entry_check_tests)
    add_test(NAME entry_check_tests COMMAND entry_check_tests)

    add_executable(interleaver_tests
        tests/interleaver_test.cpp
    )

    target_compile_features(interleaver_tests PRIVATE cxx_std_20)
    target_link_libraries(interleaver_tests PRIVATE order_book_reference)
    order_book_enable_warnings(interleaver_tests)
    order_book_enable_sanitizers(interleaver_tests)
    add_test(NAME interleaver_tests COMMAND interleaver_tests)

    add_executable(replay_engine_tests
        tests/replay_engine_test.cpp
    )

    target_compile_features(replay_engine_tests PRIVATE cxx_std_20)
    target_link_libraries(replay_engine_tests PRIVATE order_book_replay)
    order_book_enable_warnings(replay_engine_tests)
    order_book_enable_sanitizers(replay_engine_tests)
    # Also replays every sample file in tests/replay/.
    add_test(NAME replay_engine_tests COMMAND replay_engine_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay)

    # order_book_cli on each sample, compared byte for byte with tests/replay/expected/. Paths
    # are relative to tests/replay, so diagnostics don't depend on the checkout location.
    # A stream with no expected file must stay empty.
    function(order_book_add_replay_cli_test name)
        cmake_parse_arguments(PARSE_ARGV 1 cli "" "EXIT;STDOUT;STDERR;CLEAN" "ARGS")
        set(dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay)
        list(JOIN cli_ARGS "|" args)
        set(defines -DNAME=${name} -DCLI=$<TARGET_FILE:order_book_cli> -DARGS=${args}
                    -DWORKING_DIRECTORY=${dir} -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/replay-cli-output
                    -DEXPECTED_EXIT=${cli_EXIT})
        if(cli_CLEAN)
            list(APPEND defines -DCLEAN=${cli_CLEAN})
        endif()
        if(cli_STDOUT)
            list(APPEND defines -DEXPECTED_STDOUT=${dir}/expected/${cli_STDOUT})
        endif()
        if(cli_STDERR)
            list(APPEND defines -DEXPECTED_STDERR=${dir}/expected/${cli_STDERR})
        endif()
        add_test(NAME replay_cli_${name} COMMAND ${CMAKE_COMMAND} ${defines} -P ${dir}/check_cli_output.cmake)
    endfunction()

    # --events --book prints everything: each event, the statistics, and the final book.
    foreach(sample basic price_time_priority sweep_and_rest book_rejections sequence_gaps limits
                   leading_zeros header_only crlf_line_endings no_final_newline v2_header_only market_orders
                   ioc_fok post_only self_trade_prevention stops replace)
        order_book_add_replay_cli_test(${sample}
            ARGS replay valid/${sample}.csv --events --book EXIT 0 STDOUT ${sample}.stdout)
    endforeach()
    # Output selection: the statistics alone by default; --trades, --events (which covers the
    # trades), and --book add to them. Options go before or after the file, and "--" ends them.
    order_book_add_replay_cli_test(statistics_only ARGS replay valid/basic.csv EXIT 0 STDOUT basic_statistics.stdout)
    order_book_add_replay_cli_test(trades ARGS replay valid/basic.csv --trades EXIT 0 STDOUT basic_trades.stdout)
    order_book_add_replay_cli_test(book ARGS replay valid/basic.csv --book EXIT 0 STDOUT basic_book.stdout)
    order_book_add_replay_cli_test(trades_and_book
        ARGS replay valid/price_time_priority.csv --trades --book EXIT 0 STDOUT price_time_priority_trades_book.stdout)
    order_book_add_replay_cli_test(trades_none_executed
        ARGS replay valid/book_rejections.csv --trades EXIT 0 STDOUT book_rejections_statistics.stdout)
    order_book_add_replay_cli_test(events_cover_trades
        ARGS replay valid/basic.csv --trades --events --book EXIT 0 STDOUT basic.stdout)
    order_book_add_replay_cli_test(options_before_file
        ARGS replay --book --events valid/basic.csv EXIT 0 STDOUT basic.stdout)
    order_book_add_replay_cli_test(end_of_options
        ARGS replay --events --book -- valid/basic.csv EXIT 0 STDOUT basic.stdout)
    # Validation changes nothing on a sound book.
    order_book_add_replay_cli_test(validate_per_command
        ARGS replay valid/basic.csv --events --book --validate per-command EXIT 0 STDOUT basic.stdout)
    order_book_add_replay_cli_test(validate_none
        ARGS replay --validate none valid/basic.csv --events --book EXIT 0 STDOUT basic.stdout)
    # A parse error part-way prints what ran, then the diagnostic; one before any command, or
    # a file that can't be opened, prints the empty book's statistics.
    order_book_add_replay_cli_test(price_zero
        ARGS replay malformed/price_zero.csv --events --book EXIT 1 STDOUT price_zero.stdout STDERR price_zero.stderr)
    order_book_add_replay_cli_test(price_zero_statistics
        ARGS replay malformed/price_zero.csv EXIT 1 STDOUT price_zero_statistics.stdout STDERR price_zero.stderr)
    order_book_add_replay_cli_test(empty_file
        ARGS replay malformed/empty_file.csv --book EXIT 1 STDOUT header_only.stdout STDERR empty_file.stderr)
    # v2: cascade trades are tagged with the line that ran the cascade; a combination the row
    # alone decides is a parse error.
    order_book_add_replay_cli_test(stops_trades ARGS replay valid/stops.csv --trades EXIT 0 STDOUT stops_trades.stdout)
    order_book_add_replay_cli_test(market_gtc
        ARGS replay malformed/market_gtc.csv EXIT 1 STDOUT market_gtc_statistics.stdout STDERR market_gtc.stderr)
    order_book_add_replay_cli_test(missing_file
        ARGS replay no_such_file.csv EXIT 1 STDOUT empty_statistics.stdout STDERR missing_file.stderr)
    # Help goes to stdout and exits 0, wherever the option appears before "--".
    order_book_add_replay_cli_test(help_long ARGS --help EXIT 0 STDOUT help.stdout)
    order_book_add_replay_cli_test(help_short ARGS -h EXIT 0 STDOUT help.stdout)
    order_book_add_replay_cli_test(help_command ARGS help EXIT 0 STDOUT help.stdout)
    order_book_add_replay_cli_test(help_after_options
        ARGS replay valid/basic.csv --trades --bogus --help EXIT 0 STDOUT help.stdout)
    order_book_add_replay_cli_test(help_after_end_of_options
        ARGS replay -- --help EXIT 1 STDOUT empty_statistics.stdout STDERR help_as_file.stderr)
    order_book_add_replay_cli_test(demo ARGS demo EXIT 0 STDOUT demo.stdout)
    # A log and its CSV twin print the same bytes: both are checked against one expected file.
    order_book_add_replay_cli_test(log_twin_log
        ARGS replay log/every_command.oblog --events --book EXIT 0 STDOUT log_every_command.stdout)
    order_book_add_replay_cli_test(log_twin_csv
        ARGS replay log/every_command.csv --events --book EXIT 0 STDOUT log_every_command.stdout)
    order_book_add_replay_cli_test(log_header_only ARGS replay log/header_only.oblog EXIT 0)
    order_book_add_replay_cli_test(log_torn_tail
        ARGS replay log/torn_tail.oblog --events EXIT 1 STDOUT log_torn_tail.stdout STDERR log_torn_tail.stderr)
    # v3 and --symbol: one section per symbol, sorted by symbol bytes. v1 and v2 stay unchanged.
    order_book_add_replay_cli_test(v3_two_instruments
        ARGS replay valid/v3_two_instruments.csv --events --book EXIT 0 STDOUT v3_two_instruments.stdout)
    order_book_add_replay_cli_test(v3_one_instrument
        ARGS replay valid/v3_one_instrument.csv --events --book EXIT 0 STDOUT v3_one_instrument.stdout)
    order_book_add_replay_cli_test(v3_no_statistics
        ARGS replay valid/v3_two_instruments.csv --events --no-statistics EXIT 0 STDOUT v3_two_instruments_no_statistics.stdout)
    order_book_add_replay_cli_test(v3_header_only ARGS replay valid/v3_header_only.csv EXIT 0)
    order_book_add_replay_cli_test(symbol_stands_in
        ARGS replay valid/basic.csv --symbol AAPL --events --book EXIT 0 STDOUT basic_symbol.stdout)
    order_book_add_replay_cli_test(symbol_lower_case
        ARGS replay malformed/symbol_lower_case.csv EXIT 1 STDOUT symbol_lower_case.stdout STDERR symbol_lower_case.stderr)
    order_book_add_replay_cli_test(symbol_missing
        ARGS replay malformed/symbol_missing.csv EXIT 1 STDOUT symbol_missing.stdout STDERR symbol_missing.stderr)
    order_book_add_replay_cli_test(usage_symbol_on_v3
        ARGS replay valid/v3_one_instrument.csv --symbol AAPL EXIT 2 STDERR usage_symbol_on_v3.stderr)
    order_book_add_replay_cli_test(usage_bad_symbol
        ARGS replay valid/basic.csv --symbol aapl EXIT 2 STDERR usage_bad_symbol.stderr)
    order_book_add_replay_cli_test(usage_missing_symbol
        ARGS replay valid/basic.csv --symbol EXIT 2 STDERR usage_missing_symbol.stderr)
    # Usage errors name the problem, then print the usage, on stderr only.
    order_book_add_replay_cli_test(usage_no_arguments EXIT 2 STDERR usage_no_arguments.stderr)
    order_book_add_replay_cli_test(usage_unknown_command
        ARGS play valid/basic.csv EXIT 2 STDERR usage_unknown_command.stderr)
    order_book_add_replay_cli_test(usage_missing_file ARGS replay --trades EXIT 2 STDERR usage_missing_file.stderr)
    # Several inputs: v1 and v2 files run one at a time; files that name symbols run together.
    order_book_add_replay_cli_test(two_v1_files
        ARGS replay valid/header_only.csv valid/v2_header_only.csv EXIT 0 STDOUT two_header_only.stdout)
    order_book_add_replay_cli_test(two_v3_files
        ARGS replay valid/v3_two_instruments.csv log/every_command.oblog --events --no-statistics
        EXIT 0 STDOUT two_named_inputs.stdout)
    order_book_add_replay_cli_test(directory_of_logs ARGS replay log/shards --no-statistics --events EXIT 0 STDOUT shard_logs.stdout)
    # A directory leaves out files being made and cut records, and one file keeps "line N:".
    order_book_add_replay_cli_test(directory_skips_cut_records
        ARGS replay log/cut --events --no-statistics EXIT 0 STDOUT cut_directory.stdout)
    order_book_add_replay_cli_test(cut_records_read_directly
        ARGS replay log/cut/cut-0000-00000000000000000002.oblog --events --no-statistics
        EXIT 0 STDOUT cut_records.stdout)
    order_book_add_replay_cli_test(logs_not_from_zero
        ARGS replay log/shards/shard0-part2.oblog EXIT 1 STDERR logs_not_from_zero.stderr)
    order_book_add_replay_cli_test(logs_with_a_gap
        ARGS replay log/shards/shard0-part1.oblog log/gap/shard0-part3.oblog EXIT 1 STDERR logs_with_a_gap.stderr)
    order_book_add_replay_cli_test(symbol_in_two_shards
        ARGS replay log/shards log/moved/shard2-aapl.oblog EXIT 1 STDERR symbol_in_two_shards.stderr)
    order_book_add_replay_cli_test(usage_convert_no_out ARGS convert valid/basic.csv EXIT 2 STDERR usage_convert_no_out.stderr)
    order_book_add_replay_cli_test(usage_convert_no_input ARGS convert --out somewhere EXIT 2 STDERR usage_convert_no_input.stderr)
    order_book_add_replay_cli_test(usage_convert_needs_symbol
        ARGS convert valid/basic.csv --out ${CMAKE_CURRENT_BINARY_DIR}/no-such-output.oblog
        EXIT 2 STDERR usage_convert_needs_symbol.stderr)
    order_book_add_replay_cli_test(usage_convert_symbol_on_v3
        ARGS convert valid/v3_one_instrument.csv --symbol AAPL --out ${CMAKE_CURRENT_BINARY_DIR}/no-such-output.oblog
        EXIT 2 STDERR usage_convert_symbol_on_v3.stderr)
    order_book_add_replay_cli_test(usage_convert_unknown_option
        ARGS convert valid/basic.csv --outt somewhere EXIT 2 STDERR usage_convert_unknown_option.stderr)
    order_book_add_replay_cli_test(usage_convert_bad_symbol
        ARGS convert valid/basic.csv --symbol aapl --out somewhere EXIT 2 STDERR usage_bad_symbol.stderr)
    order_book_add_replay_cli_test(usage_convert_repeated_out
        ARGS convert valid/basic.csv --out one --out two EXIT 2 STDERR usage_convert_repeated_out.stderr)
    order_book_add_replay_cli_test(usage_repeated_flag
        ARGS replay valid/basic.csv --trades --trades EXIT 2 STDERR usage_repeated_flag.stderr)
    set(convert_output ${CMAKE_CURRENT_BINARY_DIR}/replay-cli-convert)
    order_book_add_replay_cli_test(convert_csv_to_log
        ARGS convert log/every_command.csv --out ${convert_output}-csv.oblog EXIT 0 CLEAN ${convert_output}-csv.oblog)
    order_book_add_replay_cli_test(convert_log_to_csv
        ARGS convert log/every_command.oblog --out ${convert_output}-log.csv EXIT 0 CLEAN ${convert_output}-log.csv)
    order_book_add_replay_cli_test(convert_placed_log
        ARGS convert log/placement.oblog --out ${convert_output}-placed.csv EXIT 0 CLEAN ${convert_output}-placed.csv)
    order_book_add_replay_cli_test(convert_directory
        ARGS convert log/shards --out ${convert_output}-directory EXIT 0 CLEAN ${convert_output}-directory)
    order_book_add_replay_cli_test(convert_damaged_input
        ARGS convert log/torn_tail.oblog --out ${convert_output}-damaged.csv
        EXIT 1 STDERR log_torn_tail.stderr CLEAN ${convert_output}-damaged.csv)
    order_book_add_replay_cli_test(convert_missing_input
        ARGS convert no_such_file.csv --out ${convert_output}-missing.oblog EXIT 1 STDERR missing_file.stderr)
    order_book_add_replay_cli_test(convert_existing_output
        ARGS convert valid/v3_one_instrument.csv --out valid/basic.csv EXIT 1 STDERR convert_existing_output.stderr)
    order_book_add_replay_cli_test(usage_unknown_option
        ARGS replay valid/basic.csv --trade EXIT 2 STDERR usage_unknown_option.stderr)
    order_book_add_replay_cli_test(usage_missing_validation
        ARGS replay valid/basic.csv --validate EXIT 2 STDERR usage_missing_validation.stderr)
    order_book_add_replay_cli_test(usage_bad_validation
        ARGS replay valid/basic.csv --validate sometimes EXIT 2 STDERR usage_bad_validation.stderr)
    order_book_add_replay_cli_test(usage_demo_argument ARGS demo valid/basic.csv EXIT 2 STDERR usage_demo_argument.stderr)
    # send: everything decided before connecting. What it prints over a connection is
    # engine_end_to_end_tests'.
    order_book_add_replay_cli_test(usage_send_no_file ARGS send --connect 127.0.0.1:9000 EXIT 2 STDERR usage_send_no_file.stderr)
    order_book_add_replay_cli_test(usage_send_two_files
        ARGS send valid/basic.csv valid/limits.csv --connect 127.0.0.1:9000 EXIT 2 STDERR usage_send_two_files.stderr)
    order_book_add_replay_cli_test(usage_send_no_connect
        ARGS send valid/basic.csv --symbol AAPL EXIT 2 STDERR usage_send_no_connect.stderr)
    order_book_add_replay_cli_test(usage_send_missing_connect_value
        ARGS send valid/basic.csv --connect EXIT 2 STDERR usage_send_missing_connect_value.stderr)
    order_book_add_replay_cli_test(usage_send_bad_address
        ARGS send valid/basic.csv --connect localhost:9000 EXIT 2 STDERR usage_send_bad_address.stderr)
    order_book_add_replay_cli_test(usage_send_port_zero
        ARGS send valid/basic.csv --connect 127.0.0.1:0 EXIT 2 STDERR usage_send_port_zero.stderr)
    order_book_add_replay_cli_test(usage_send_repeated_connect
        ARGS send valid/basic.csv --connect 127.0.0.1:9000 --connect 127.0.0.1:9001 EXIT 2 STDERR usage_send_repeated_connect.stderr)
    order_book_add_replay_cli_test(usage_send_unknown_option
        ARGS send valid/basic.csv --events --connect 127.0.0.1:9000 EXIT 2 STDERR usage_send_unknown_option.stderr)
    order_book_add_replay_cli_test(usage_send_needs_symbol
        ARGS send valid/basic.csv --connect 127.0.0.1:9000 EXIT 2 STDERR usage_send_needs_symbol.stderr)
    order_book_add_replay_cli_test(usage_send_symbol_on_v3
        ARGS send valid/v3_two_instruments.csv --symbol AAPL --connect 127.0.0.1:9000 EXIT 2 STDERR usage_send_symbol_on_v3.stderr)
    order_book_add_replay_cli_test(send_missing_file
        ARGS send no_such_file.csv --symbol AAPL --connect 127.0.0.1:9000 EXIT 1 STDERR missing_file.stderr)

    # Randomized command testing: seeded streams of submissions and cancellations, each command
    # checked against the book's invariants and a reference model. The harness is a library so
    # its own tests can drive it.
    add_library(order_book_randomized STATIC
        tests/randomized/abort_reporter.cpp
        tests/randomized/command.cpp
        tests/randomized/command_generator.cpp
        tests/randomized/command_replayer.cpp
        tests/randomized/failure_report.cpp
        tests/randomized/shrink.cpp
    )

    target_compile_features(order_book_randomized PUBLIC cxx_std_20)
    target_include_directories(order_book_randomized PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/randomized)
    target_link_libraries(order_book_randomized PUBLIC order_book order_book_reference)
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

    # The engine's harness: the real entry check and Shard beside a model and a witness per
    # instrument, in one thread (docs/internals/randomized-tests.md).
    add_library(order_book_engine_testing STATIC
        tests/engine/connection_streams.cpp
        tests/engine/engine_harness.cpp
        tests/engine/gateway_rig.cpp
        tests/engine/gateway_stepper.cpp
        tests/engine/threaded_run.cpp
    )

    target_compile_features(order_book_engine_testing PUBLIC cxx_std_20)
    target_include_directories(order_book_engine_testing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/engine)
    target_link_libraries(order_book_engine_testing PUBLIC order_book_randomized order_book_network)
    order_book_enable_warnings(order_book_engine_testing)
    order_book_enable_sanitizers(order_book_engine_testing)

    add_executable(engine_randomized_tests
        tests/engine/engine_main.cpp
    )

    target_compile_features(engine_randomized_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_randomized_tests PRIVATE order_book_engine_testing)
    order_book_enable_warnings(engine_randomized_tests)
    order_book_enable_sanitizers(engine_randomized_tests)
    add_test(NAME engine_randomized_seeds COMMAND engine_randomized_tests)
    add_test(NAME engine_stepper_cli COMMAND engine_randomized_tests --stepper stop --seed 7 --commands 200)
    # A shrunk reproduction's form: a schedule on chosen lines of another seed's stream.
    add_test(NAME engine_stepper_lines_cli
        COMMAND engine_randomized_tests --stepper run --seed 3 --stream-seed 9 --commands 200 --instruments 8 --shards 2
                --gateway-threads 2 --connections 8 --placement pinned --lines 0,5,17,40,41,42,150)
    add_test(NAME engine_threads_cli
        COMMAND engine_randomized_tests --threads run --seed 3 --commands 300 --shards 2 --gateway-threads 2 --ring-slots 2)
    set_tests_properties(engine_threads_cli PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)

    add_executable(engine_harness_tests
        tests/engine/harness_test.cpp
    )

    target_compile_features(engine_harness_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_harness_tests PRIVATE order_book_engine_testing)
    order_book_enable_warnings(engine_harness_tests)
    order_book_enable_sanitizers(engine_harness_tests)
    add_test(NAME engine_harness_tests COMMAND engine_harness_tests)

    add_executable(gateway_tests
        tests/gateway_test.cpp
    )

    target_compile_features(gateway_tests PRIVATE cxx_std_20)
    target_link_libraries(gateway_tests PRIVATE order_book_engine_testing)
    order_book_enable_warnings(gateway_tests)
    order_book_enable_sanitizers(gateway_tests)
    add_test(NAME gateway_tests COMMAND gateway_tests)
    set_tests_properties(gateway_tests PROPERTIES TIMEOUT 240)

    # Layer 3: real gateway and matching threads. A lost wake-up hangs, so the timeout fails it.
    add_executable(engine_thread_tests
        tests/engine_thread_test.cpp
    )

    target_compile_features(engine_thread_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_thread_tests PRIVATE order_book_engine_testing)
    order_book_enable_warnings(engine_thread_tests)
    order_book_enable_sanitizers(engine_thread_tests)
    add_test(NAME engine_thread_tests COMMAND engine_thread_tests)
    set_tests_properties(engine_thread_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 600)

    add_executable(gateway_socket_tests
        tests/gateway_socket_test.cpp
    )

    target_compile_features(gateway_socket_tests PRIVATE cxx_std_20)
    target_link_libraries(gateway_socket_tests PRIVATE order_book_network)
    order_book_enable_warnings(gateway_socket_tests)
    order_book_enable_sanitizers(gateway_socket_tests)
    add_test(NAME gateway_socket_tests COMMAND gateway_socket_tests)
    set_tests_properties(gateway_socket_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)

    add_executable(engine_settings_tests
        tests/engine_settings_test.cpp
    )

    target_compile_features(engine_settings_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_settings_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(engine_settings_tests)
    order_book_enable_sanitizers(engine_settings_tests)
    add_test(NAME engine_settings_tests COMMAND engine_settings_tests)

    add_executable(thread_placement_tests
        tests/thread_placement_test.cpp
    )

    target_compile_features(thread_placement_tests PRIVATE cxx_std_20)
    target_link_libraries(thread_placement_tests PRIVATE order_book_runtime)
    order_book_enable_warnings(thread_placement_tests)
    order_book_enable_sanitizers(thread_placement_tests)
    add_test(NAME thread_placement_tests COMMAND thread_placement_tests)
    set_tests_properties(thread_placement_tests PROPERTIES LABELS threaded)

    add_executable(engine_tests
        tests/engine_test.cpp
    )

    target_compile_features(engine_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_tests PRIVATE order_book_network)
    order_book_enable_warnings(engine_tests)
    order_book_enable_sanitizers(engine_tests)
    add_test(NAME engine_tests COMMAND engine_tests)
    set_tests_properties(engine_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)

    # Replaces operator new, so it is its own executable.
    add_executable(engine_start_allocation_tests
        tests/engine_start_allocation_test.cpp
    )

    target_compile_features(engine_start_allocation_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_start_allocation_tests PRIVATE order_book_network)
    order_book_enable_warnings(engine_start_allocation_tests)
    order_book_enable_sanitizers(engine_start_allocation_tests)
    add_test(NAME engine_start_allocation_tests COMMAND engine_start_allocation_tests)
    set_tests_properties(engine_start_allocation_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

    add_executable(engine_process_tests
        tests/engine_process_test.cpp
    )

    target_compile_features(engine_process_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_process_tests PRIVATE order_book_process_testing order_book_network)
    order_book_enable_warnings(engine_process_tests)
    order_book_enable_sanitizers(engine_process_tests)
    add_test(NAME engine_process_tests COMMAND engine_process_tests $<TARGET_FILE:order_book_engine>)
    set_tests_properties(engine_process_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 180)

    # Real signals and the abort handler, so it builds apps/' handlers in with the test.
    add_executable(engine_signal_tests
        tests/engine_signal_test.cpp
        apps/engine_handlers.cpp
    )

    target_compile_features(engine_signal_tests PRIVATE cxx_std_20)
    target_include_directories(engine_signal_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(engine_signal_tests PRIVATE order_book_process_testing order_book_network)
    order_book_enable_warnings(engine_signal_tests)
    order_book_enable_sanitizers(engine_signal_tests)
    add_dependencies(engine_signal_tests order_book_engine)
    add_test(NAME engine_signal_tests COMMAND engine_signal_tests $<TARGET_FILE:order_book_engine>)
    set_tests_properties(engine_signal_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads SKIP_RETURN_CODE 77 TIMEOUT 180)

    add_executable(engine_end_to_end_tests
        tests/engine_end_to_end_test.cpp
    )

    target_compile_features(engine_end_to_end_tests PRIVATE cxx_std_20)
    target_link_libraries(engine_end_to_end_tests PRIVATE order_book_process_testing order_book_network)
    order_book_enable_warnings(engine_end_to_end_tests)
    order_book_enable_sanitizers(engine_end_to_end_tests)
    add_dependencies(engine_end_to_end_tests order_book_engine order_book_cli)
    add_test(NAME engine_end_to_end_tests
        COMMAND engine_end_to_end_tests $<TARGET_FILE:order_book_engine> $<TARGET_FILE:order_book_cli>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/engine/samples)
    set_tests_properties(engine_end_to_end_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 300)

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
    foreach(command addOrder submitOrder cancelOrder consumeLevel replaceOrder)
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
    # Workload generation, timed runs, and result export (benchmarks/README.md). A library so
    # its own tests can drive it. Numbers are only meaningful from a Release build; the
    # program refuses any other unless given --allow-unoptimized.
    add_library(order_book_benchmarking STATIC
        benchmarks/workload.cpp
        benchmarks/behavior_workload.cpp
        benchmarks/benchmark_runner.cpp
        benchmarks/benchmark_report.cpp
        benchmarks/component_benchmark.cpp
        benchmarks/engine_process.cpp
    )

    find_package(Threads REQUIRED)
    target_compile_features(order_book_benchmarking PUBLIC cxx_std_20)
    target_include_directories(order_book_benchmarking PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
    target_link_libraries(order_book_benchmarking PUBLIC order_book_network order_book_reference order_book_process_testing
                                                         Threads::Threads)
    # Recorded with every result, so a report says which configuration produced it.
    target_compile_definitions(order_book_benchmarking PRIVATE ORDER_BOOK_BUILD_CONFIG="$<CONFIG>")
    # GCC defines no macro under UBSan, so say whether order_book_enable_sanitizers applies it.
    if(ORDER_BOOK_ENABLE_UBSAN AND NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_definitions(order_book_benchmarking PRIVATE ORDER_BOOK_UNDEFINED_SANITIZER=1)
    endif()
    order_book_enable_warnings(order_book_benchmarking)
    order_book_enable_sanitizers(order_book_benchmarking)

    add_executable(order_book_benchmarks
        benchmarks/order_book_benchmark.cpp
    )

    target_compile_features(order_book_benchmarks PRIVATE cxx_std_20)
    target_link_libraries(order_book_benchmarks PRIVATE order_book_benchmarking)
    order_book_enable_warnings(order_book_benchmarks)
    order_book_enable_sanitizers(order_book_benchmarks)
    # The engine kind runs the order_book_engine beside it.
    add_dependencies(order_book_benchmarks order_book_engine)

    # The latency harness (benchmarks/latency/README.md): clock, histogram, timetable driver,
    # and report. A library so latency_harness_tests can drive it with a fake clock and target.
    add_library(order_book_latency STATIC
        benchmarks/latency/handoff_target.cpp
        benchmarks/latency/round_trip_target.cpp
        benchmarks/latency/clock.cpp
        benchmarks/latency/command_target.cpp
        benchmarks/latency/histogram.cpp
        benchmarks/latency/latency_report.cpp
        benchmarks/latency/thread_setup.cpp
        benchmarks/latency/timetable.cpp
    )

    target_compile_features(order_book_latency PUBLIC cxx_std_20)
    target_include_directories(order_book_latency PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/latency)
    target_link_libraries(order_book_latency PUBLIC order_book_benchmarking)
    order_book_enable_warnings(order_book_latency)
    order_book_enable_sanitizers(order_book_latency)

    add_executable(order_book_latency_benchmarks
        benchmarks/latency/latency_main.cpp
    )

    target_compile_features(order_book_latency_benchmarks PRIVATE cxx_std_20)
    target_link_libraries(order_book_latency_benchmarks PRIVATE order_book_latency)
    add_dependencies(order_book_latency_benchmarks order_book_engine)  # the round trip runs the engine beside it
    order_book_enable_warnings(order_book_latency_benchmarks)
    order_book_enable_sanitizers(order_book_latency_benchmarks)

    # One operation in bulk (benchmarks/README.md, "Component costs"). It takes thread_setup
    # from order_book_latency, so its pins mean what the latency harness's do.
    add_executable(order_book_component_benchmarks
        benchmarks/component_benchmark_main.cpp
    )

    target_compile_features(order_book_component_benchmarks PRIVATE cxx_std_20)
    target_link_libraries(order_book_component_benchmarks PRIVATE order_book_latency)
    order_book_enable_warnings(order_book_component_benchmarks)
    order_book_enable_sanitizers(order_book_component_benchmarks)

    if(BUILD_TESTING)
        add_executable(latency_harness_tests
            tests/latency_harness_test.cpp
        )

        target_compile_features(latency_harness_tests PRIVATE cxx_std_20)
        target_link_libraries(latency_harness_tests PRIVATE order_book_latency)
        order_book_enable_warnings(latency_harness_tests)
        order_book_enable_sanitizers(latency_harness_tests)
        add_test(NAME latency_harness_tests
            COMMAND latency_harness_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/latency)

        # The program end to end on tiny runs, closed-loop then on a timetable, writing its
        # files. --allow-unoptimized, since CTest runs in every configuration.
        add_test(NAME latency_cli_smoke
            COMMAND order_book_latency_benchmarks --workload small-book --commands 500 --runs 2 --warmup 1
                    --noise-seconds 0 --allow-unoptimized --out-dir ${CMAKE_CURRENT_BINARY_DIR}/latency-smoke)
        add_test(NAME latency_cli_smoke_handoff
            COMMAND order_book_latency_benchmarks --interval hand-off --workload small-book --commands 500 --runs 2 --warmup 1
                    --noise-seconds 0 --allow-unoptimized --out-dir ${CMAKE_CURRENT_BINARY_DIR}/latency-smoke-handoff)
        set_tests_properties(latency_cli_smoke_handoff PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)
        # The shipped engine as a child on port 0: closed-loop, then the open-loop ladder with its
        # two driver threads, with every command kept. The ladder's rates come from the capacity
        # its own build measures, so a sanitizer build's 10% row is slow: it is kept small, with
        # no warm-up, since GCC ASan+UBSan Debug took 190 s of 300 at 500 commands and one.
        add_test(NAME latency_cli_smoke_round_trip
            COMMAND order_book_latency_benchmarks --interval round-trip --engine $<TARGET_FILE:order_book_engine>
                    --workload small-book --commands 500 --runs 2 --warmup 1 --discard-seconds 0 --noise-seconds 0
                    --allow-unoptimized --out-dir ${CMAKE_CURRENT_BINARY_DIR}/latency-smoke-round-trip)
        add_test(NAME latency_cli_smoke_round_trip_response
            COMMAND order_book_latency_benchmarks --interval round-trip --engine $<TARGET_FILE:order_book_engine>
                    --workload small-book --commands 300 --runs 2 --warmup 0 --time response --discard-seconds 0
                    --noise-seconds 0 --allow-unoptimized --out-dir ${CMAKE_CURRENT_BINARY_DIR}/latency-smoke-round-trip-response)
        set_tests_properties(latency_cli_smoke_round_trip latency_cli_smoke_round_trip_response PROPERTIES
            LABELS threaded RESOURCE_LOCK threads TIMEOUT 300)
        add_test(NAME latency_cli_smoke_response
            COMMAND order_book_latency_benchmarks --workload small-book --commands 500 --runs 2 --warmup 1
                    --time response --noise-seconds 0 --allow-unoptimized
                    --out-dir ${CMAKE_CURRENT_BINARY_DIR}/latency-smoke-response)

        # Both groups on tiny structures, with two threads through the controls.
        add_test(NAME component_cli_smoke
            COMMAND order_book_component_benchmarks --size smoke --threads 2 --allow-unoptimized
                    --csv ${CMAKE_CURRENT_BINARY_DIR}/component-smoke.csv)

        add_executable(benchmark_harness_tests
            tests/benchmark_harness_test.cpp
        )

        target_compile_features(benchmark_harness_tests PRIVATE cxx_std_20)
        target_link_libraries(benchmark_harness_tests PRIVATE order_book_benchmarking)
        order_book_enable_warnings(benchmark_harness_tests)
        order_book_enable_sanitizers(benchmark_harness_tests)
        add_test(NAME benchmark_harness_tests COMMAND benchmark_harness_tests)

        add_executable(benchmark_queue_tests
            tests/benchmark_queue_test.cpp
        )

        target_compile_features(benchmark_queue_tests PRIVATE cxx_std_20)
        target_link_libraries(benchmark_queue_tests PRIVATE order_book_benchmarking)
        order_book_enable_warnings(benchmark_queue_tests)
        order_book_enable_sanitizers(benchmark_queue_tests)
        add_test(NAME benchmark_queue_tests COMMAND benchmark_queue_tests)
        set_tests_properties(benchmark_queue_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 300)

        add_executable(benchmark_gateway_tests
            tests/benchmark_gateway_test.cpp
        )

        target_compile_features(benchmark_gateway_tests PRIVATE cxx_std_20)
        target_link_libraries(benchmark_gateway_tests PRIVATE order_book_benchmarking)
        order_book_enable_warnings(benchmark_gateway_tests)
        order_book_enable_sanitizers(benchmark_gateway_tests)
        add_test(NAME benchmark_gateway_tests COMMAND benchmark_gateway_tests)
        set_tests_properties(benchmark_gateway_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 300)

        add_executable(benchmark_engine_tests
            tests/benchmark_engine_test.cpp
        )

        target_compile_features(benchmark_engine_tests PRIVATE cxx_std_20)
        target_link_libraries(benchmark_engine_tests PRIVATE order_book_benchmarking)
        order_book_enable_warnings(benchmark_engine_tests)
        order_book_enable_sanitizers(benchmark_engine_tests)
        add_dependencies(benchmark_engine_tests order_book_engine)
        add_test(NAME benchmark_engine_tests COMMAND benchmark_engine_tests $<TARGET_FILE:order_book_engine>)
        set_tests_properties(benchmark_engine_tests PROPERTIES LABELS threaded RESOURCE_LOCK threads TIMEOUT 300)

        # Exactly zero heap calls on every workload, where only a checked build's own checks
        # could allocate.
        add_executable(matching_path_allocation_tests
            tests/matching_path_allocation_test.cpp
        )

        target_compile_features(matching_path_allocation_tests PRIVATE cxx_std_20)
        target_link_libraries(matching_path_allocation_tests PRIVATE order_book_benchmarking)
        order_book_enable_warnings(matching_path_allocation_tests)
        order_book_enable_sanitizers(matching_path_allocation_tests)
        add_test(NAME matching_path_allocation_tests COMMAND matching_path_allocation_tests)
        set_tests_properties(matching_path_allocation_tests PROPERTIES SKIP_RETURN_CODE 77)

        add_test(NAME benchmark_queue_smoke
            COMMAND order_book_benchmarks --workload small-book --commands 2000 --kind queue --instruments 4
                    --repetitions 1 --warmup 0 --allow-unoptimized)
        # It must exit 1, for the prediction's reason, not for any other.
        add_test(NAME benchmark_queue_tampered
            COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:order_book_benchmarks> -DEXPECTED_EXIT=1
                    "-DARGS=--workload|small-book|--commands|2000|--kind|queue|--tamper-prediction|--repetitions|1|--warmup|0|--allow-unoptimized"
                    "-DOUTPUT_REGEX=but the workload predicted" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_failure.cmake)
        set_tests_properties(benchmark_queue_smoke benchmark_queue_tampered PROPERTIES
            LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)
        # Real loopback sockets on port 0, which ThreadSanitizer and Arm run.
        add_test(NAME benchmark_gateway_smoke
            COMMAND order_book_benchmarks --workload small-book --commands 2000 --kind gateway --instruments 4
                    --repetitions 1 --warmup 0 --allow-unoptimized)
        add_test(NAME benchmark_gateway_tampered
            COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:order_book_benchmarks> -DEXPECTED_EXIT=1
                    "-DARGS=--workload|small-book|--commands|2000|--kind|gateway|--tamper-prediction|--repetitions|1|--warmup|0|--allow-unoptimized"
                    "-DOUTPUT_REGEX=but the workload predicted" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_failure.cmake)
        set_tests_properties(benchmark_gateway_smoke benchmark_gateway_tampered PROPERTIES
            LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)
        # The shipped engine as a child on port 0, with no log until the log writer lands.
        add_test(NAME benchmark_engine_smoke
            COMMAND order_book_benchmarks --workload small-book --commands 2000 --kind engine --instruments 4 --shards 2
                    --engine $<TARGET_FILE:order_book_engine> --repetitions 1 --warmup 0 --allow-unoptimized)
        add_test(NAME benchmark_engine_tampered
            COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:order_book_benchmarks> -DEXPECTED_EXIT=1
                    "-DARGS=--workload|small-book|--commands|2000|--kind|engine|--engine|$<TARGET_FILE:order_book_engine>|--tamper-prediction|--repetitions|1|--warmup|0|--allow-unoptimized"
                    "-DOUTPUT_REGEX=but the workload predicted" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_failure.cmake)
        set_tests_properties(benchmark_engine_smoke benchmark_engine_tampered PROPERTIES
            LABELS threaded RESOURCE_LOCK threads TIMEOUT 120)
        add_test(NAME benchmark_shard_smoke
            COMMAND order_book_benchmarks --workload small-book --commands 2000 --kind shard --instruments 4
                    --repetitions 1 --warmup 0 --allow-unoptimized)
        # The program end to end on a tiny run of one workload, both kinds, writing both
        # files. --allow-unoptimized, since CTest runs in every configuration.
        add_test(NAME benchmark_cli_smoke
            COMMAND order_book_benchmarks --workload small-book --commands 500 --kind both
                    --repetitions 2 --warmup 1 --allow-unoptimized
                    --csv ${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke.csv
                    --json ${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke.json)
    endif()

    if(ORDER_BOOK_BUILD_PROFILING)
        # Profiling tools (benchmarks/profiling/README.md): exact allocation counts per
        # command, and on Windows a sampling profiler for the benchmark program. Not tested.
        add_executable(allocation_profile benchmarks/profiling/allocation_profile.cpp)
        target_compile_features(allocation_profile PRIVATE cxx_std_20)
        target_link_libraries(allocation_profile PRIVATE order_book_benchmarking)
        order_book_enable_warnings(allocation_profile)
        if(WIN32)
            add_executable(sampling_profiler benchmarks/profiling/sampling_profiler.cpp)
            target_compile_features(sampling_profiler PRIVATE cxx_std_20)
            target_link_libraries(sampling_profiler PRIVATE dbghelp)
            order_book_enable_warnings(sampling_profiler)
        endif()
    endif()
endif()
