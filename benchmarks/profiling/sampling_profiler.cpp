// A sampling profiler for one process's main thread, Windows x64 only.
//
// It starts the command given after "--", and until that process exits it suspends the
// process's main thread at a fixed interval, reads the thread's registers, walks its stack
// with dbghelp (inline frames included, so a function the compiler inlined still shows up),
// and resumes it. Nothing runs inside the profiled process: the sampler's own work happens
// in this process, so the target's heap lock is never taken while its thread is suspended.
// Symbols are resolved after the run from the target's PDB (build the target with /Zi and
// link it with /DEBUG; see benchmarks/profiling/README.md).
//
// Output: <prefix>.folded holds every sample as a root-to-leaf stack and its count, the
// format flame graph tools read. <prefix>.txt is a report over the samples selected with
// --root and --drop: self and inclusive time by function, self time by source line, time
// spent in the heap allocator by the project function that called into it, and time in the
// standard containers (std::map, std::unordered_map, std::list, std::vector, strings and
// streams) by entry point and caller.
//
// Usage: sampling_profiler [--interval-us N] [--out PREFIX] [--root NAME]... [--drop NAME]...
//                          -- command [args...]
// A sample is selected when its stack has a frame whose name contains one of the --root
// names (any, if none is given) and no frame containing a --drop name.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct RawFrame {
    std::uint64_t address;
    std::uint32_t inlineContext;
};

// One sample is frames[begin, begin + count), leaf first.
struct Sample {
    std::size_t begin;
    std::size_t count;
};

struct Options {
    unsigned intervalMicroseconds = 500;
    std::string outputPrefix = "profile";
    std::vector<std::string> roots;
    std::vector<std::string> drops;
    std::wstring commandLine;
    std::wstring program;
};

std::string narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) {
        out += c < 128 ? static_cast<char>(c) : '?';
    }
    return out;
}

std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (wchar_t c : argument) {
        if (c == L'"') {
            quoted += L'\\';
        }
        quoted += c;
    }
    quoted += L'"';
    return quoted;
}

bool parseOptions(int argc, wchar_t** argv, Options& options) {
    int i = 1;
    for (; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--") {
            ++i;
            break;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "option %s needs a value\n", narrow(argument).c_str());
            return false;
        }
        const std::wstring value = argv[++i];
        if (argument == L"--interval-us") {
            options.intervalMicroseconds = static_cast<unsigned>(std::stoul(value));
        } else if (argument == L"--out") {
            options.outputPrefix = narrow(value);
        } else if (argument == L"--root") {
            options.roots.push_back(narrow(value));
        } else if (argument == L"--drop") {
            options.drops.push_back(narrow(value));
        } else {
            std::fprintf(stderr, "unknown option %s\n", narrow(argument).c_str());
            return false;
        }
    }
    if (i >= argc) {
        std::fprintf(stderr, "usage: sampling_profiler [--interval-us N] [--out PREFIX] [--root NAME]... "
                             "[--drop NAME]... -- command [args...]\n");
        return false;
    }
    options.program = argv[i];
    for (; i < argc; ++i) {
        if (!options.commandLine.empty()) {
            options.commandLine += L' ';
        }
        options.commandLine += quoteArgument(argv[i]);
    }
    return true;
}

// Waits intervalMicroseconds between samples. A high-resolution waitable timer where the
// system has one (Windows 10 1803 on), else Sleep at the 1 ms timer resolution.
class IntervalTimer {
public:
    explicit IntervalTimer(unsigned microseconds) : interval(microseconds) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    ~IntervalTimer() {
        if (timer != nullptr) {
            CloseHandle(timer);
        }
    }
    void wait() {
        if (timer != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(interval) * 10;  // relative, 100 ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
        Sleep(std::max(1u, interval / 1000));
    }

private:
    unsigned interval;
    HANDLE timer = nullptr;
};

struct Collector {
    std::vector<RawFrame> frames;
    std::vector<Sample> samples;
    std::size_t failedContexts = 0;
    std::size_t suspendedMicroseconds = 0;  // total time the target thread was held
};

// The target's stack, copied while its thread is suspended, so the walk (which is slow: it
// reads unwind data and resolves inline frames) runs after the thread has been resumed.
struct StackSnapshot {
    std::uint64_t base = 0;
    std::vector<unsigned char> bytes;
    HANDLE process = nullptr;
};

StackSnapshot* currentSnapshot = nullptr;

BOOL CALLBACK readFromSnapshot(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD bytesRead) {
    const StackSnapshot* snapshot = currentSnapshot;
    if (snapshot != nullptr && address >= snapshot->base && address + size <= snapshot->base + snapshot->bytes.size()) {
        std::memcpy(buffer, snapshot->bytes.data() + (address - snapshot->base), size);
        *bytesRead = size;
        return TRUE;
    }
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &read);
    *bytesRead = static_cast<DWORD>(read);
    return ok;
}

std::uint64_t microsecondsNow() {
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(counter.QuadPart) * 1000000 / static_cast<std::uint64_t>(frequency.QuadPart);
}

void takeSample(HANDLE process, HANDLE thread, Collector& collector, StackSnapshot& snapshot) {
    const std::uint64_t suspendedAt = microsecondsNow();
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return;
    }
    CONTEXT context;
    std::memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
    const bool haveContext = GetThreadContext(thread, &context) != 0;
    if (haveContext) {
        // Copy the committed stack from the stack pointer to the end of its region (the
        // base of the stack), which is a few kilobytes for this program.
        MEMORY_BASIC_INFORMATION region;
        std::memset(&region, 0, sizeof(region));
        std::uint64_t end = context.Rsp + 65536;
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(context.Rsp), &region, sizeof(region)) != 0) {
            end = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
        }
        snapshot.base = context.Rsp;
        snapshot.bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(end - context.Rsp, 1u << 20)));
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(context.Rsp), snapshot.bytes.data(),
                               snapshot.bytes.size(), &read)) {
            snapshot.bytes.resize(static_cast<std::size_t>(read));
        }
        snapshot.process = process;
    }
    ResumeThread(thread);
    collector.suspendedMicroseconds += static_cast<std::size_t>(microsecondsNow() - suspendedAt);
    if (!haveContext) {
        ++collector.failedContexts;
        return;
    }

    STACKFRAME_EX frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.StackFrameSize = sizeof(frame);
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    currentSnapshot = &snapshot;
    const std::size_t begin = collector.frames.size();
    for (int depth = 0; depth < 256; ++depth) {
        if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, readFromSnapshot,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr, SYM_STKWALK_DEFAULT)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        collector.frames.push_back(RawFrame{frame.AddrPC.Offset, frame.InlineFrameContext});
    }
    currentSnapshot = nullptr;
    collector.samples.push_back(Sample{begin, collector.frames.size() - begin});
}

struct FrameName {
    std::string function;   // "module!function" for modules other than the profiled program
    std::string location;   // "dir/file.cpp:line", or empty
};

std::string shortenPath(const char* path) {
    std::string text = path == nullptr ? "" : path;
    std::replace(text.begin(), text.end(), '\\', '/');
    std::size_t cut = text.rfind('/');
    if (cut != std::string::npos && cut > 0) {
        const std::size_t previous = text.rfind('/', cut - 1);
        cut = previous == std::string::npos ? 0 : previous + 1;
        return text.substr(cut);
    }
    return text;
}

class Symbolizer {
public:
    Symbolizer(HANDLE process, std::string programModule) : process(process), programModule(std::move(programModule)) {}

    const FrameName& resolve(const RawFrame& frame, bool leaf) {
        // A caller's address is a return address, so look up the instruction before it,
        // which is the call. The leaf's address is the instruction that was executing.
        const std::uint64_t lookup = leaf ? frame.address : frame.address - 1;
        const auto key = std::make_pair(lookup, frame.inlineContext);
        auto found = cache.find(key);
        if (found != cache.end()) {
            return found->second;
        }

        FrameName name;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        std::memset(symbol, 0, sizeof(SYMBOL_INFO));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromInlineContext(process, lookup, frame.inlineContext, &displacement, symbol)) {
            name.function = symbol->Name;
        } else {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(frame.address));
            name.function = hex;
        }

        IMAGEHLP_LINE64 line;
        std::memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromInlineContext(process, lookup, frame.inlineContext, 0, &lineDisplacement, &line)) {
            name.location = shortenPath(line.FileName) + ":" + std::to_string(line.LineNumber);
        }

        const std::string module = moduleName(lookup);
        if (!module.empty() && _stricmp(module.c_str(), programModule.c_str()) != 0) {
            name.function = module + "!" + name.function;
        }
        return cache.emplace(key, std::move(name)).first->second;
    }

private:
    std::string moduleName(std::uint64_t address) {
        const DWORD64 base = SymGetModuleBase64(process, address);
        if (base == 0) {
            return "";
        }
        auto found = modules.find(base);
        if (found != modules.end()) {
            return found->second;
        }
        IMAGEHLP_MODULE64 info;
        std::memset(&info, 0, sizeof(info));
        info.SizeOfStruct = sizeof(info);
        std::string name;
        if (SymGetModuleInfo64(process, address, &info)) {
            name = info.ModuleName;
        }
        return modules.emplace(base, name).first->second;
    }

    HANDLE process;
    std::string programModule;
    std::map<std::pair<std::uint64_t, std::uint32_t>, FrameName> cache;
    std::unordered_map<std::uint64_t, std::string> modules;
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string_view withoutModule(std::string_view function) {
    const std::size_t bang = function.find('!');
    return bang == std::string_view::npos ? function : function.substr(bang + 1);
}

bool isSystemFrame(std::string_view function) {
    return function.find('!') != std::string_view::npos;
}

bool isStdFrame(std::string_view function) {
    return withoutModule(function).substr(0, 5) == "std::";
}

enum class HeapKind { NONE, ALLOCATE, FREE };

HeapKind heapKind(std::string_view function) {
    const std::string_view name = withoutModule(function);
    if (name == "free" || name == "_free_base" || contains(name, "operator delete") || contains(name, "FreeHeap")
        || contains(name, "HeapFree") || contains(name, "RtlpFree")) {
        return HeapKind::FREE;
    }
    if (name == "malloc" || name == "realloc" || name == "_malloc_base" || name == "_realloc_base"
        || contains(name, "operator new") || contains(name, "AllocateHeap") || contains(name, "HeapAlloc")
        || contains(name, "RtlpAllocate") || contains(name, "LowFragHeap") || contains(name, "RtlpHeap")
        || contains(name, "RtlpLfh") || contains(name, "RtlSizeHeap")) {
        return HeapKind::ALLOCATE;
    }
    return HeapKind::NONE;
}

struct ContainerSet {
    const char* label;
    std::vector<std::string_view> markers;
};

const std::vector<ContainerSet>& containerSets() {
    static const std::vector<ContainerSet> sets = {
        {"std::map (red-black tree)", {"std::_Tree", "std::map<"}},
        {"std::unordered_map (hash table)", {"std::_Hash", "std::unordered_map", "std::_Uhash", "std::hash<", "std::_Fnv1a"}},
        {"std::list", {"std::_List", "std::list<"}},
        {"std::vector", {"std::vector<", "std::_Vector"}},
        {"std::variant / std::visit", {"std::visit", "std::variant", "std::_Variant"}},
        {"strings and streams", {"std::basic_string", "std::getline", "std::basic_istream", "std::basic_streambuf",
                                 "std::basic_stringbuf", "std::char_traits", "std::to_string", "std::_UIntegral_to_buff"}},
    };
    return sets;
}

bool inSet(std::string_view function, const ContainerSet& set) {
    const std::string_view name = withoutModule(function);
    for (std::string_view marker : set.markers) {
        if (contains(name, marker)) {
            return true;
        }
    }
    return false;
}

bool isProjectFrame(std::string_view function) {
    return !isSystemFrame(function) && !isStdFrame(function) && heapKind(function) == HeapKind::NONE;
}

struct Counter {
    std::map<std::string, std::size_t> counts;
    void add(const std::string& key, std::size_t n = 1) { counts[key] += n; }
    std::vector<std::pair<std::string, std::size_t>> sorted() const {
        std::vector<std::pair<std::string, std::size_t>> rows(counts.begin(), counts.end());
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return rows;
    }
};

void writeTable(std::ofstream& out, const std::string& title, const Counter& counter, std::size_t total,
                std::size_t limit, const char* columns) {
    out << "## " << title << "\n\n";
    out << "| samples | % | " << columns << " |\n|---:|---:|---|\n";
    std::size_t shown = 0;
    for (const auto& [key, count] : counter.sorted()) {
        if (shown++ == limit) {
            break;
        }
        char percent[32];
        std::snprintf(percent, sizeof(percent), "%.1f",
                      total == 0 ? 0.0 : 100.0 * static_cast<double>(count) / static_cast<double>(total));
        out << "| " << count << " | " << percent << " | " << key << " |\n";
    }
    out << "\n";
}

struct ResolvedSample {
    std::vector<const FrameName*> frames;  // leaf first
    std::size_t count = 1;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    STARTUPINFOW startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof(process));
    std::wstring commandLine = options.commandLine;  // CreateProcessW may modify it
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &process)) {
        std::fprintf(stderr, "cannot start %s (error %lu)\n", narrow(options.commandLine).c_str(), GetLastError());
        return 1;
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    // Attach before the process runs, while it is known to exist (a short-lived target can
    // be gone before an attach after the fact); its modules are enumerated as it loads them.
    if (!SymInitialize(process.hProcess, nullptr, FALSE)) {
        std::fprintf(stderr, "SymInitialize failed (error 0x%lx)\n", GetLastError());
        TerminateProcess(process.hProcess, 1);
        return 1;
    }
    ResumeThread(process.hThread);

    Collector collector;
    StackSnapshot snapshot;
    IntervalTimer timer(options.intervalMicroseconds);
    const ULONGLONG startedAt = GetTickCount64();
    ULONGLONG lastRefresh = 0;
    while (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
        timer.wait();
        // The program's DLLs load during its first milliseconds, so refresh the module list
        // often at first, then once a second in case one loads later.
        const ULONGLONG now = GetTickCount64();
        if (now - lastRefresh > (collector.samples.size() < 64 ? 2 : 1000)) {
            SymRefreshModuleList(process.hProcess);
            lastRefresh = now;
        }
        takeSample(process.hProcess, process.hThread, collector, snapshot);
    }
    SymRefreshModuleList(process.hProcess);
    const ULONGLONG elapsedMilliseconds = GetTickCount64() - startedAt;
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);

    std::fprintf(stderr, "profiled %s: %zu samples in %llu ms, exit code %lu\n", narrow(options.commandLine).c_str(),
                 collector.samples.size(), elapsedMilliseconds, exitCode);

    // Resolve every frame, then aggregate identical stacks.
    std::wstring programName = options.program;
    const std::size_t slash = programName.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        programName = programName.substr(slash + 1);
    }
    if (programName.size() > 4 && _wcsicmp(programName.c_str() + programName.size() - 4, L".exe") == 0) {
        programName.resize(programName.size() - 4);
    }
    Symbolizer symbolizer(process.hProcess, narrow(programName));

    std::map<std::vector<const FrameName*>, std::size_t> stacks;
    for (const Sample& sample : collector.samples) {
        std::vector<const FrameName*> frames;
        frames.reserve(sample.count);
        for (std::size_t i = 0; i < sample.count; ++i) {
            frames.push_back(&symbolizer.resolve(collector.frames[sample.begin + i], i == 0));
        }
        ++stacks[frames];
    }
    SymCleanup(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::vector<ResolvedSample> resolved;
    for (const auto& [frames, count] : stacks) {
        resolved.push_back(ResolvedSample{frames, count});
    }

    // Folded stacks, root first.
    {
        std::ofstream folded(options.outputPrefix + ".folded", std::ios::binary);
        for (const ResolvedSample& sample : resolved) {
            for (std::size_t i = sample.frames.size(); i-- > 0;) {
                folded << sample.frames[i]->function;
                if (i > 0) {
                    folded << ';';
                }
            }
            folded << ' ' << sample.count << '\n';
        }
    }

    // Selection.
    auto selected = [&](const ResolvedSample& sample) {
        bool rooted = options.roots.empty();
        for (const FrameName* frame : sample.frames) {
            for (const std::string& drop : options.drops) {
                if (contains(frame->function, drop)) {
                    return false;
                }
            }
            for (const std::string& root : options.roots) {
                if (contains(frame->function, root)) {
                    rooted = true;
                }
            }
        }
        return rooted;
    };

    std::size_t totalSamples = 0;
    std::size_t selectedSamples = 0;
    Counter selfByFunction;
    Counter inclusiveByFunction;
    Counter selfByLine;
    Counter selfByModule;
    Counter heapByCaller;
    Counter heapByKind;
    Counter heapSelf;
    std::vector<Counter> setInclusive(containerSets().size());
    std::vector<Counter> setSelf(containerSets().size());
    std::vector<std::size_t> setTotals(containerSets().size(), 0);
    Counter unselectedByRoot;

    for (const ResolvedSample& sample : resolved) {
        totalSamples += sample.count;
        if (!selected(sample)) {
            // Say what the unselected time was: the --drop frame that excluded it, or else
            // the project function nearest the root, other than main and the CRT's entry.
            std::string reason = "(none)";
            for (const FrameName* frame : sample.frames) {
                for (const std::string& drop : options.drops) {
                    if (contains(frame->function, drop)) {
                        reason = "dropped: " + frame->function;
                    }
                }
            }
            if (reason == "(none)") {
                for (std::size_t i = sample.frames.size(); i-- > 0;) {
                    const std::string& function = sample.frames[i]->function;
                    if (isProjectFrame(function) && function != "main" && function != "invoke_main"
                        && !contains(function, "mainCRTStartup") && !contains(function, "scrt_common_main")) {
                        reason = function;
                        break;
                    }
                }
            }
            unselectedByRoot.add(reason, sample.count);
            continue;
        }
        selectedSamples += sample.count;
        if (sample.frames.empty()) {
            selfByFunction.add("(no frames)", sample.count);
            continue;
        }

        const FrameName& leaf = *sample.frames[0];
        selfByFunction.add(leaf.function, sample.count);
        selfByLine.add(leaf.location.empty() ? "(no line)  " + leaf.function : leaf.location + "  " + leaf.function,
                       sample.count);
        {
            const std::size_t bang = leaf.function.find('!');
            selfByModule.add(bang == std::string::npos ? "(program)" : leaf.function.substr(0, bang), sample.count);
        }

        std::vector<std::string_view> seen;
        for (const FrameName* frame : sample.frames) {
            if (std::find(seen.begin(), seen.end(), std::string_view(frame->function)) == seen.end()) {
                seen.push_back(frame->function);
                inclusiveByFunction.add(frame->function, sample.count);
            }
        }

        // Heap: the leaf-most heap frame decides the kind; the project frame nearest the leaf
        // is the caller, and the frame just above it the standard-library entry point.
        HeapKind kind = HeapKind::NONE;
        for (const FrameName* frame : sample.frames) {
            kind = heapKind(frame->function);
            if (kind != HeapKind::NONE) {
                break;
            }
        }
        if (kind != HeapKind::NONE) {
            heapByKind.add(kind == HeapKind::ALLOCATE ? "allocate" : "free", sample.count);
            heapSelf.add(leaf.function, sample.count);
            std::string caller = "(none)";
            std::string entry = "(direct)";
            for (std::size_t i = 0; i < sample.frames.size(); ++i) {
                if (isProjectFrame(sample.frames[i]->function)) {
                    caller = sample.frames[i]->function;
                    if (i > 0) {
                        entry = sample.frames[i - 1]->function;
                    }
                    break;
                }
            }
            heapByCaller.add(std::string(kind == HeapKind::ALLOCATE ? "allocate" : "free") + " | " + caller + " | "
                                 + entry,
                             sample.count);
        }

        // Containers: for each set, is any frame in it? Attribute to the entry frame (the
        // set's frame nearest the root) and the project frame below it.
        for (std::size_t s = 0; s < containerSets().size(); ++s) {
            const ContainerSet& set = containerSets()[s];
            std::size_t entryIndex = sample.frames.size();
            for (std::size_t i = sample.frames.size(); i-- > 0;) {
                if (inSet(sample.frames[i]->function, set)) {
                    entryIndex = i;
                    break;
                }
            }
            if (entryIndex == sample.frames.size()) {
                continue;
            }
            setTotals[s] += sample.count;
            std::string caller = "(none)";
            for (std::size_t i = entryIndex + 1; i < sample.frames.size(); ++i) {
                if (isProjectFrame(sample.frames[i]->function)) {
                    caller = sample.frames[i]->function;
                    break;
                }
            }
            setInclusive[s].add(sample.frames[entryIndex]->function + " | " + caller, sample.count);
            setSelf[s].add(leaf.function, sample.count);
        }
    }

    std::ofstream report(options.outputPrefix + ".txt", std::ios::binary);
    report << "# Sampling profile\n\n";
    report << "command: " << narrow(options.commandLine) << "\n";
    report << "exit code: " << exitCode << "\n";
    report << "interval: " << options.intervalMicroseconds << " us; wall time " << elapsedMilliseconds
           << " ms; the target thread was suspended for " << collector.suspendedMicroseconds / 1000
           << " ms of that\n";
    report << "samples: " << totalSamples << " total, " << selectedSamples << " selected";
    if (!options.roots.empty()) {
        report << "; root:";
        for (const std::string& root : options.roots) {
            report << " " << root;
        }
    }
    if (!options.drops.empty()) {
        report << "; drop:";
        for (const std::string& drop : options.drops) {
            report << " " << drop;
        }
    }
    report << "\n";
    if (collector.failedContexts != 0) {
        report << "failed to read the thread context " << collector.failedContexts << " times\n";
    }
    report << "\nPercentages are of the selected samples.\n\n";

    writeTable(report, "Unselected samples, by the project function nearest their root", unselectedByRoot,
               totalSamples - selectedSamples, 15, "function");
    writeTable(report, "Self time by module", selfByModule, selectedSamples, 20, "module");
    writeTable(report, "Self time by function (inlined functions shown as their own frames)", selfByFunction,
               selectedSamples, 50, "function");
    writeTable(report, "Inclusive time by function", inclusiveByFunction, selectedSamples, 60, "function");
    writeTable(report, "Self time by source line", selfByLine, selectedSamples, 60, "location  function");
    writeTable(report, "Heap allocator: samples inside malloc/free and the heap, by kind", heapByKind,
               selectedSamples, 5, "kind");
    writeTable(report, "Heap allocator: by project caller and the standard-library entry point it went through",
               heapByCaller, selectedSamples, 40, "kind | project caller | entry point");
    writeTable(report, "Heap allocator: self time by function", heapSelf, selectedSamples, 25, "function");
    for (std::size_t s = 0; s < containerSets().size(); ++s) {
        const std::string title = std::string("Container: ") + containerSets()[s].label + " (inclusive "
                                  + std::to_string(setTotals[s]) + " samples)";
        writeTable(report, title + ": by entry point and project caller", setInclusive[s], selectedSamples, 30,
                   "entry point | project caller");
        writeTable(report, title + ": self time by function", setSelf[s], selectedSamples, 20, "function");
    }
    std::fprintf(stderr, "wrote %s.txt and %s.folded\n", options.outputPrefix.c_str(), options.outputPrefix.c_str());
    return 0;
}
