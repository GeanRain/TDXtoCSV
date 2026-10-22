// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
#include <cstdio>
#include <stdio.h>
#include <string>
#include <windows.h>
#include <fstream>
#include <cstdlib>
#include <mutex>
 
#include "common.h"
#include "utils/logger.h"
#include "utils/ini_reader.h"
#include "export/async_writer.h"

std::string config_file_dir;
// DLL 独占此句柄；卸载时关闭，Windows 终止其中所有后台进程。
HANDLE backend_job = nullptr;
std::uint64_t backend_lifetime = 0;
bool reset_backend_lifetime() noexcept
{
    if (backend_job) CloseHandle(backend_job);
    backend_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    LARGE_INTEGER generation{};
    if (!backend_job || !SetInformationJobObject(backend_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !QueryPerformanceCounter(&generation) || generation.QuadPart <= 0)
    {
        if (backend_job) CloseHandle(backend_job);
        backend_job = nullptr;
        return false;
    }
    backend_lifetime = static_cast<std::uint64_t>(generation.QuadPart);
    return true;
}
const char *kDefaultIni =
    R"ini(
[log]
mode=file
level=1
file=saveAsCSV.log

[export]
csv_dir=
symbol_width=6
default_time=150000

[async]
queue_capacity=1000
queue_memory_mb=2048
worker_threads=8
worker_path=SaveAsCSVWorker.exe
)ini";


BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD reason,
                      LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char ddl_path[MAX_PATH];
        GetModuleFileNameA(hModule, ddl_path, MAX_PATH);
        std::string path(ddl_path);
        size_t p = path.find_last_of("\\/");
        config_file_dir.reserve(MAX_PATH);
        config_file_dir.assign((p == std::string::npos) ? path : path.substr(0, p));
        Logger::initialize(config_file_dir);
        if (!reset_backend_lifetime())
        {
            Logger::shutdown();
            return FALSE;
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (backend_job) CloseHandle(backend_job);
        backend_job = nullptr;
        if (reserved == nullptr)
        {
            tdx_export::dispose_export_writer();
            Logger::shutdown();
        }
    }
    return TRUE;
}
