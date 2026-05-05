#include "app/crash_dump.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

#if defined(_WIN32)
static std::string gCrashContext = "PFighter raylib startup";

static std::string crashTimestampForFile() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d_%H%M%S");
    return out.str();
}

void setCrashContext(std::string context) {
    gCrashContext = std::move(context);
}

static LONG WINAPI pfighterUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    try {
        const std::filesystem::path crashDir = std::filesystem::current_path() / "engine_cpp" / "crashes";
        std::filesystem::create_directories(crashDir);
        const std::string stamp = crashTimestampForFile();
        const std::filesystem::path dumpPath = crashDir / ("pfighter_raylib_" + stamp + ".dmp");
        const std::filesystem::path textPath = crashDir / ("pfighter_raylib_" + stamp + ".txt");

        HANDLE dumpFile = CreateFileA(
            dumpPath.string().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (dumpFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exceptionData{};
            exceptionData.ThreadId = GetCurrentThreadId();
            exceptionData.ExceptionPointers = exceptionInfo;
            exceptionData.ClientPointers = FALSE;
            MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                dumpFile,
                MiniDumpWithDataSegs,
                exceptionInfo ? &exceptionData : nullptr,
                nullptr,
                nullptr);
            CloseHandle(dumpFile);
        }

        std::ofstream report(textPath, std::ios::binary);
        report << "PFighter raylib crash\n";
        report << "dump=" << dumpPath.string() << "\n";
        if (exceptionInfo && exceptionInfo->ExceptionRecord) {
            report << "exception_code=0x"
                   << std::hex << exceptionInfo->ExceptionRecord->ExceptionCode << std::dec << "\n";
            report << "exception_address=" << exceptionInfo->ExceptionRecord->ExceptionAddress << "\n";
        }
        report << "\nlast_context:\n" << gCrashContext << "\n";
    } catch (...) {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashDumpHandler() {
    SetUnhandledExceptionFilter(pfighterUnhandledExceptionFilter);
}
#else
void installCrashDumpHandler() {}
void setCrashContext(std::string) {}
#endif
