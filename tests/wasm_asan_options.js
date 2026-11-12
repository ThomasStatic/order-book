// Linked with --pre-js into every test executable of an Emscripten ASan build (CMakeLists.txt).
// Emscripten's ASan reads its options from Module, not the environment. It unwinds a JavaScript
// stack for each allocation and free it records, which made the suites 50 times slower than
// without, too slow for CI; with no allocation stacks a report still names the bad access and
// where it happened, only not where the memory was allocated or freed.
Module['ASAN_OPTIONS'] = Module['ASAN_OPTIONS'] || 'malloc_context_size=0';
