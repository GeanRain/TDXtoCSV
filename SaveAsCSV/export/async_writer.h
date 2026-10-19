#pragma once

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tdx_export
{
using ExportClock = std::chrono::steady_clock;
constexpr std::size_t kMaxWorkers = 128;

// 完整持有一次保存所需数据，后台不访问调用线程的暂存区。
struct ExportJob
{
    int count = 0;
    int native_period = 0;
    int default_time = 150000;
    bool minute = false;
    std::string symbol;
    std::string period;
    std::string csv_dir;
    std::vector<float> dates; // 通达信原始 DATE，转换和内容校验在后台完成。
    std::vector<float> times; // 分钟周期的原始 HHMM；长周期不使用。
    std::vector<float> series[6];
    ExportClock::time_point started{}, queued{};
    DWORD caller_thread = 0;
    std::uint64_t id = 0;
    double call_ms[10]{};
    double save_ms[4]{};
    double save_call_ms = 0;
    double queue_wait_ms = 0;
    double frontend_elapsed_ms = 0; // 相对时长；跨进程不传C++时钟对象。
};

inline std::uint64_t job_memory(const ExportJob &job) noexcept
{
    std::uint64_t bytes = sizeof(ExportJob) + job.symbol.capacity() +
        job.period.capacity() + job.csv_dir.capacity() + 3;
    bytes += static_cast<std::uint64_t>(job.dates.capacity() + job.times.capacity()) * sizeof(float);
    for (const auto &series : job.series)
        bytes += static_cast<std::uint64_t>(series.capacity()) * sizeof(float);
    return bytes;
}

struct AsyncLimits
{
    std::size_t capacity = 1000;
    std::uint64_t bytes = 2048ULL * 1024 * 1024;
    std::size_t workers = 8; // 同时保存数，允许 1—128。
};

struct WriterStatus
{
    std::uint64_t accepted = 0, succeeded = 0, failed = 0, rejected = 0;
    std::uint64_t reserved = 0, unfinished = 0, bytes = 0, waiting = 0;
    std::uint64_t busy = 0;
    bool running = false, accepting = false;
};

class AsyncWriter
{
public:
    using Saver = bool (*)(ExportJob &);
    using MemoryProbe = bool (*)(std::uint64_t &total, std::uint64_t &available,
                                std::uint64_t &commit_available);
    explicit AsyncWriter(Saver save, MemoryProbe probe = nullptr);
    ~AsyncWriter();
    AsyncWriter(const AsyncWriter &) = delete;
    AsyncWriter &operator=(const AsyncWriter &) = delete;

    void start(AsyncLimits limits = {}, const std::string &csv_dir = {});
    std::uint64_t reserve(std::uint64_t bytes);
    std::uint64_t try_reserve(std::uint64_t bytes); // 资源不足返回0，供独立后台管理通道使用。
    void check_memory(std::uint64_t additional);
    void wait_allocation(); // 实际申请失败后等待 200 毫秒；仍响应停止接收。
    void release(std::uint64_t reservation) noexcept;
    void submit(std::uint64_t reservation, ExportJob &&job);
    void note_rejected() noexcept;
    WriterStatus status();
    bool finish();
    void cancel_input() noexcept;
    void stop_input() noexcept; // 宿主退出：停止接收，不把退出本身算作数据错误。
    std::string cancel_event_name();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// DllMain 只保存模块标识；线程和模块引用在普通调用中管理。
void set_module(HMODULE module) noexcept;
void dispose_export_writer() noexcept;
} // namespace tdx_export
