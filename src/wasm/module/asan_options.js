// Linked with --pre-js into order_book_wasm_module in an Emscripten ASan build (CMakeLists.txt),
// in place of tests/wasm_asan_options.js, whose reason it shares: no allocation stacks, which made
// ASan far too slow. The module keeps the shipped 128 MiB, which never grows, so ASan's quarantine
// of freed memory, 64 MiB by default on wasm32, is cut to 16 MiB: at the default, the worst-case
// session ran out of memory under ASan alone.
Module['ASAN_OPTIONS'] = Module['ASAN_OPTIONS'] || 'malloc_context_size=0:quarantine_size_mb=16';
