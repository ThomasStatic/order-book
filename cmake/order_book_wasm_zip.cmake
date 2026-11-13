# Builds order_book_wasm-<release>.zip (docs/internals/wasm.md, "The zip"), run by the
# order_book_wasm_zip target as `cmake -P` with:
#   SOURCE   the repository, for git and the worker and client sources
#   BINARY   the build directory, which holds the module and the configured client
#   RELEASE  v and the project version, as the zip's name has it
#   PROTOCOL the message protocol's version
# It stages the six files in BINARY/order_book_wasm-zip, writes manifest.json there, and zips
# them flat into BINARY/order_book_wasm-<RELEASE>.zip.
foreach(required SOURCE BINARY RELEASE PROTOCOL)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "order_book_wasm_zip.cmake needs -D${required}=...")
    endif()
endforeach()

set(stage "${BINARY}/order_book_wasm-zip")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")
set(files
    "${BINARY}/order_book_wasm.js"
    "${BINARY}/order_book_wasm.wasm"
    "${SOURCE}/src/wasm/js/engine-worker.js"
    "${BINARY}/engine-client/engine-client.js"
    "${BINARY}/engine-client/engine-client.d.ts")
foreach(file IN LISTS files)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "the zip needs ${file}, which hasn't been built")
    endif()
    file(COPY "${file}" DESTINATION "${stage}")
endforeach()

# The tag is what git describe says: a tagged commit's tag, or the commit past the last tag.
find_package(Git QUIET)
set(tag "unknown")
set(commit "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty WORKING_DIRECTORY "${SOURCE}"
                    OUTPUT_VARIABLE tag OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE described ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${SOURCE}"
                    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE parsed ERROR_QUIET)
    if(NOT described EQUAL 0)
        set(tag "unknown")
    endif()
    if(NOT parsed EQUAL 0)
        set(commit "unknown")
    endif()
endif()

set(entries "")
set(names order_book_wasm.js order_book_wasm.wasm engine-worker.js engine-client.js engine-client.d.ts)
foreach(name IN LISTS names)
    file(SHA256 "${stage}/${name}" hash)
    if(entries)
        string(APPEND entries ",\n")
    endif()
    string(APPEND entries "    \"${name}\": \"${hash}\"")
endforeach()
file(WRITE "${stage}/manifest.json"
     "{\n  \"release\": \"${RELEASE}\",\n  \"tag\": \"${tag}\",\n  \"commit\": \"${commit}\",\n  \"protocol\": ${PROTOCOL},\n  \"files\": {\n${entries}\n  }\n}\n")

set(zip "${BINARY}/order_book_wasm-${RELEASE}.zip")
file(REMOVE "${zip}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${zip}" --format=zip ${names} manifest.json
                WORKING_DIRECTORY "${stage}" RESULT_VARIABLE zipped)
if(NOT zipped EQUAL 0)
    message(FATAL_ERROR "could not write ${zip}")
endif()
message(STATUS "order_book_wasm: ${zip} (${tag})")
