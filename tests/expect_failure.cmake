# Runs a program that must fail in one exact way: with EXPECTED_EXIT, or, with EXPECTED_EXIT unset,
# any failure, and printing something that matches OUTPUT_REGEX. CTest's PASS_REGULAR_EXPRESSION
# ignores the exit status, and WILL_FAIL ignores the reason, so neither proves this alone.
# Invoked by CTest with -D for PROGRAM, ARGS (separated by '|'), OUTPUT_REGEX, and EXPECTED_EXIT.
string(REPLACE "|" ";" args "${ARGS}")
execute_process(
    COMMAND "${PROGRAM}" ${args}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

if(DEFINED EXPECTED_EXIT)
    if(NOT actual_exit STREQUAL EXPECTED_EXIT)
        message(FATAL_ERROR "FAILED: expected exit ${EXPECTED_EXIT}, got '${actual_exit}':\n${output}")
    endif()
elseif(actual_exit STREQUAL "0")
    message(FATAL_ERROR "FAILED: expected a failure, and it exited 0:\n${output}")
endif()
if(NOT output MATCHES "${OUTPUT_REGEX}")
    message(FATAL_ERROR "FAILED: expected output matching '${OUTPUT_REGEX}', got:\n${output}")
endif()
