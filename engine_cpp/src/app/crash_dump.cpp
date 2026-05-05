#include "app/crash_dump.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <typeinfo>
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

static std::filesystem::path crashDirectory() {
    return std::filesystem::current_path() / "engine_cpp" / "crashes";
}

static std::string currentExceptionDescription() {
    std::exception_ptr exception = std::current_exception();
    if (!exception) {
        return "none";
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::bad_alloc& e) {
        return std::string{"std::bad_alloc: "} + e.what();
    } catch (const std::exception& e) {
        return std::string{typeid(e).name()} + ": " + e.what();
    } catch (...) {
        return "unknown non-std exception";
    }
}

void setCrashContext(std::string context) {
    gCrashContext = std::move(context);
}

static LONG WINAPI pfighterUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    try {
        const std::filesystem::path crashDir = crashDirectory();
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

static void pfighterTerminateHandler() {
    try {
        const std::filesystem::path crashDir = crashDirectory();
        std::filesystem::create_directories(crashDir);
        const std::string stamp = crashTimestampForFile();
        const std::filesystem::path textPath = crashDir / ("pfighter_raylib_" + stamp + "_terminate.txt");

        std::ofstream report(textPath, std::ios::binary);
        report << "PFighter raylib terminated\n";
        report << "exception=" << currentExceptionDescription() << "\n";
        report << "\nlast_context:\n" << gCrashContext << "\n";
    } catch (...) {
    }
    std::abort();
}

void installCrashDumpHandler() {
    SetUnhandledExceptionFilter(pfighterUnhandledExceptionFilter);
    std::set_terminate(pfighterTerminateHandler);
}
#else
void installCrashDumpHandler() {}
void setCrashContext(std::string) {}
#endif
