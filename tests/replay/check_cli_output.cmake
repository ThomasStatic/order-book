# Runs `order_book_cli <ARGS>` from WORKING_DIRECTORY and compares its exit status, stdout,
# and stderr byte for byte with the expected ones. Invoked by CTest with -D for NAME, CLI,
# ARGS (separated by '|', since add_test splits on ';'), WORKING_DIRECTORY, OUTPUT_DIR (for
# the captured output), EXPECTED_EXIT, and the optional EXPECTED_STDOUT and EXPECTED_STDERR
# files; a stream without one must be empty.
#
# Output is captured to files and compared as hex: execute_process's OUTPUT_VARIABLE and a
# plain file(READ) both drop CRs on Windows, which would hide CRLF output.
string(REPLACE "|" ";" args "${ARGS}")
set(actual_stdout_file "${OUTPUT_DIR}/${NAME}.stdout")
set(actual_stderr_file "${OUTPUT_DIR}/${NAME}.stderr")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
    COMMAND "${CLI}" ${args}
    WORKING_DIRECTORY "${WORKING_DIRECTORY}"
    RESULT_VARIABLE actual_exit
    OUTPUT_FILE "${actual_stdout_file}"
    ERROR_FILE "${actual_stderr_file}")

function(read_hex path out)
    set(content "")
    if(path)
        file(READ "${path}" content HEX)
    endif()
    set(${out} "${content}" PARENT_SCOPE)
endfunction()

set(failed FALSE)
if(NOT actual_exit STREQUAL EXPECTED_EXIT)
    message(SEND_ERROR "exit status: expected ${EXPECTED_EXIT}, got '${actual_exit}'")
    set(failed TRUE)
endif()
foreach(stream stdout stderr)
    string(TOUPPER "${stream}" upper)
    read_hex("${EXPECTED_${upper}}" expected)
    read_hex("${actual_${stream}_file}" actual)
    if(NOT actual STREQUAL expected)
        file(READ "${actual_${stream}_file}" text)
        message(SEND_ERROR "${stream} differs from '${EXPECTED_${upper}}' (empty if none); got:\n${text}")
        set(failed TRUE)
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "FAILED: order_book_cli ${args}")
endif()
