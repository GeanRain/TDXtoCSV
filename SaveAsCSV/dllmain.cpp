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
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        tdx_export::dispose_export_writer();
        Logger::shutdown();
    }
    return TRUE;
}
