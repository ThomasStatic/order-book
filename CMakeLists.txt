cmake_minimum_required(VERSION 3.20)

project(order_book VERSION 1.0.0 LANGUAGES CXX)

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
        list(JOIN sanitizers "," sanitizer_list)

        target_compile_options(${target_name} PRIVATE -fsanitize=${sanitizer_list} -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=${sanitizer_list})
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
    src/replay_parser.cpp
    src/replay_engine.cpp
    src/replay_summary.cpp
)

target_compile_features(order_book_replay PUBLIC cxx_std_20)
target_link_libraries(order_book_replay PUBLIC order_book)
order_book_enable_warnings(order_book_replay)
order_book_enable_sanitizers(order_book_replay)

add_executable(order_book_cli
    apps/order_book_cli.cpp
)

target_compile_features(order_book_cli PRIVATE cxx_std_20)
target_link_libraries(order_book_cli PRIVATE order_book_replay)
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
        cmake_parse_arguments(PARSE_ARGV 1 cli "" "EXIT;STDOUT;STDERR" "ARGS")
        set(dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay)
        list(JOIN cli_ARGS "|" args)
        set(defines -DNAME=${name} -DCLI=$<TARGET_FILE:order_book_cli> -DARGS=${args}
                    -DWORKING_DIRECTORY=${dir} -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/replay-cli-output
                    -DEXPECTED_EXIT=${cli_EXIT})
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
    # Usage errors name the problem, then print the usage, on stderr only.
    order_book_add_replay_cli_test(usage_no_arguments EXIT 2 STDERR usage_no_arguments.stderr)
    order_book_add_replay_cli_test(usage_unknown_command
        ARGS play valid/basic.csv EXIT 2 STDERR usage_unknown_command.stderr)
    order_book_add_replay_cli_test(usage_missing_file ARGS replay --trades EXIT 2 STDERR usage_missing_file.stderr)
    order_book_add_replay_cli_test(usage_two_files
        ARGS replay valid/basic.csv valid/limits.csv EXIT 2 STDERR usage_two_files.stderr)
    order_book_add_replay_cli_test(usage_unknown_option
        ARGS replay valid/basic.csv --trade EXIT 2 STDERR usage_unknown_option.stderr)
    order_book_add_replay_cli_test(usage_missing_validation
        ARGS replay valid/basic.csv --validate EXIT 2 STDERR usage_missing_validation.stderr)
    order_book_add_replay_cli_test(usage_bad_validation
        ARGS replay valid/basic.csv --validate sometimes EXIT 2 STDERR usage_bad_validation.stderr)
    order_book_add_replay_cli_test(usage_demo_argument ARGS demo valid/basic.csv EXIT 2 STDERR usage_demo_argument.stderr)

    # Randomized command testing: seeded streams of submissions and cancellations, each command
    # checked against the book's invariants and a reference model. The harness is a library so
    # its own tests can drive it.
    # The independent model of the book, shared by the randomized harness (and, later, the
    # benchmark workloads); it never touches an OrderBook.
    add_library(order_book_reference STATIC
        tests/reference/reference_book.cpp
    )
    target_compile_features(order_book_reference PUBLIC cxx_std_20)
    target_include_directories(order_book_reference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference)
    target_link_libraries(order_book_reference PUBLIC order_book)
    order_book_enable_warnings(order_book_reference)
    order_book_enable_sanitizers(order_book_reference)

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
        benchmarks/benchmark_runner.cpp
        benchmarks/benchmark_report.cpp
    )

    target_compile_features(order_book_benchmarking PUBLIC cxx_std_20)
    target_include_directories(order_book_benchmarking PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
    target_link_libraries(order_book_benchmarking PUBLIC order_book_replay)
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

    if(BUILD_TESTING)
        add_executable(benchmark_harness_tests
            tests/benchmark_harness_test.cpp
        )

        target_compile_features(benchmark_harness_tests PRIVATE cxx_std_20)
        target_link_libraries(benchmark_harness_tests PRIVATE order_book_benchmarking)
        order_book_enable_warnings(benchmark_harness_tests)
        order_book_enable_sanitizers(benchmark_harness_tests)
        add_test(NAME benchmark_harness_tests COMMAND benchmark_harness_tests)

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
