#pragma once
#include "async_writer.h"
namespace tdx_export
{
// DLL 只保留通信代理；任务与保存线程由独立 EXE 持有。
class ProcessWriter
{
public:
    explicit ProcessWriter(const std::string &directory);
    ~ProcessWriter();
    void start(AsyncLimits limits={},const std::string &csv_dir={},const std::string &worker_path={});
    std::uint64_t reserve(std::uint64_t bytes);
    void check_memory(std::uint64_t additional);
    void wait_allocation();
    void release(std::uint64_t reservation) noexcept;
    void submit(std::uint64_t reservation,ExportJob &&job);
    void note_rejected() noexcept;
    WriterStatus status();
    bool finish();
    void cancel_input() noexcept;
    std::string cancel_event_name();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
