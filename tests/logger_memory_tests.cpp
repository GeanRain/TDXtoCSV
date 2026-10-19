// 日志内存回归：只拒绝 C++ 分配，不耗尽内存；调用真实 Logger 并检查真实日志。
#include "../SaveAsCSV/utils/logger.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>

namespace allocation_fault {
bool deny = false;
}

void *operator new(std::size_t bytes) {
    if (allocation_fault::deny) throw std::bad_alloc();
    if (void *memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

const char *kDefaultIni = "[log]\nmode=file\nlevel=1\nfile=logger_memory.log\n";

namespace test {
namespace fs = std::filesystem;
int passed = 0, failed = 0;

void check(bool condition, const char *message) {
    std::printf("[%s] %s\n", condition ? "通过" : "失败", message);
    condition ? ++passed : ++failed;
}

void prepare(const fs::path &directory, int level) {
    fs::create_directory(directory);
    std::ofstream config(directory / "saveAsCVS.ini", std::ios::binary);
    config << "[log]\nmode=file\nlevel=" << level << "\nfile=logger_memory.log\n";
    config.close();
    if (!config) throw std::runtime_error("无法准备隔离日志配置");
}

bool log_without_memory(const char *level, const char *marker) noexcept {
    // 所有路径和配置均已准备完成，故障窗口内仅运行生产日志调用。
    allocation_fault::deny = true;
    try {
        Logger::log(level, "%s attempt=%u", marker, 17U);
        allocation_fault::deny = false;
        return true;
    } catch (...) {
        allocation_fault::deny = false;
        return false;
    }
}

std::string read_log(const fs::path &directory) {
    std::string contents;
    unsigned files = 0;
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".log") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        if (!input) throw std::runtime_error("无法读取隔离日志");
        contents.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        ++files;
    }
    if (files != 1) throw std::runtime_error("未找到唯一真实日志文件");
    return contents;
}
}

int main(int argc, char **argv) {
    using namespace test;
    try {
        if (argc != 2) throw std::runtime_error("用法：logger_memory_tests.exe 输出根目录");
        fs::path output = fs::absolute(argv[1]);
        fs::create_directories(output);
        const fs::path root = output / ("logger-memory-" + std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(GetTickCount64()));
        if (!fs::create_directory(root)) throw std::runtime_error("无法创建专属测试目录");
        std::printf("日志测试输出：%s\n", root.string().c_str());

        const fs::path info_directory = root / "info-level";
        prepare(info_directory, 1);
        Logger::initialize(info_directory.string());
        Logger::log("INFO", "normal_info_before_pressure value=%u", 23U);
        const bool debug_ok = log_without_memory("DEBUG", "filtered_debug_under_pressure");
        const bool warn_ok = log_without_memory("WARN", "memory_retry_warn");
        const bool error_ok = log_without_memory("ERROR", "memory_retry_error");
        Logger::log("INFO", "normal_info_after_pressure value=%u", 29U);
        Logger::shutdown();
        const std::string info = read_log(info_directory);
        check(info.find("[INFO] normal_info_before_pressure value=23\n") != std::string::npos,
            "普通 INFO 保留原有格式与参数");
        check(debug_ok && info.find("filtered_debug_under_pressure") == std::string::npos,
            "拒绝分配时 DEBUG 仍按等级过滤且不抛异常");
        check(warn_ok, "拒绝所有 C++ 分配时 WARN 不向调用方抛异常");
        check(info.find("[WARN] memory_retry_warn attempt=17\n") != std::string::npos,
            "拒绝所有 C++ 分配时 WARN 实际写入日志");
        check(error_ok, "拒绝所有 C++ 分配时 ERROR 不向调用方抛异常");
        check(info.find("[ERROR] memory_retry_error attempt=17\n") != std::string::npos,
            "拒绝所有 C++ 分配时 ERROR 实际写入日志");
        check(info.find("[INFO] normal_info_after_pressure value=29\n") != std::string::npos,
            "恢复分配后日志可以继续正常输出");

        const fs::path warn_directory = root / "warn-level";
        prepare(warn_directory, 2);
        Logger::initialize(warn_directory.string());
        const bool info_ok = log_without_memory("INFO", "filtered_info_under_pressure");
        Logger::shutdown();
        check(info_ok && read_log(warn_directory).find("filtered_info_under_pressure") == std::string::npos,
            "WARN 等级下 INFO 仍被过滤且不抛异常");
        std::printf("日志内存回归：通过 %d，失败 %d。\n", passed, failed);
        return failed ? 1 : 0;
    } catch (const std::exception &error) {
        allocation_fault::deny = false;
        std::fprintf(stderr, "日志内存测试准备失败：%s\n", error.what());
        return 2;
    }
}
