#include "async_writer.h"
#include "../common.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <new>
#include <process.h>
#include <stdexcept>
#include <system_error>

namespace tdx_export
{
namespace
{
HMODULE export_module = nullptr;

bool memory_status(std::uint64_t &total, std::uint64_t &available, std::uint64_t &commit)
{
    MEMORYSTATUSEX info{};
    info.dwLength = sizeof(info);
    if (!GlobalMemoryStatusEx(&info)) return false;
    total = info.ullTotalVirtual;
    available = info.ullAvailVirtual;
    commit = info.ullAvailPageFile;
    return true;
}

void update_status_file(const std::string &directory, const std::string &name) noexcept
{
    try
    {
        const std::filesystem::path folder(directory), target = folder / name;
        std::filesystem::create_directories(folder);
        std::filesystem::path previous;
        for (const auto &file : std::filesystem::directory_iterator(folder))
        {
            if (!file.is_regular_file()) continue;
            const auto filename = file.path().filename().string();
            if (filename == name) return;
            if (previous.empty() && (filename.rfind("_bg_", 0) == 0 || filename.rfind("bg_", 0) == 0)) previous = file.path();
        }
        if (!previous.empty())
        {
            // 不覆盖同名文件，也不改写复用文件的内容。
            if (!MoveFileExA(previous.string().c_str(), target.string().c_str(), 0))
                throw std::system_error(GetLastError(), std::system_category(), "状态文件改名失败");
        }
        else
        {
            HANDLE file = CreateFileA(target.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                throw std::system_error(GetLastError(), std::system_category(), "空状态文件创建失败");
            CloseHandle(file);
        }
    }
    catch (const std::exception &error)
    {
        try { LOG_ERROR("后台状态文件更新失败 csv_dir=%s 原因=%s", directory.c_str(), error.what()); } catch (...) {}
    }
    catch (...) { try { LOG_ERROR("后台状态文件更新失败 csv_dir=%s", directory.c_str()); } catch (...) {} }
}
}

void set_module(HMODULE module) noexcept { export_module = module; }

struct AsyncWriter::Impl
{
    struct Entry { std::unique_ptr<ExportJob> job; std::string target; std::uint64_t bytes = 0; };
    struct MemoryReading
    {
        std::uint64_t total = 0, used = 0, commit = 0, additional = 0;
        bool checked = false;
    } memory;
    struct WaitResult { double waited_ms = -1; const char *blocked_by = "none"; };
    const char *blocked_by = "unknown";
    Saver save;
    MemoryProbe probe;
    std::mutex mutex, lifecycle;
    std::condition_variable changed;
    std::deque<Entry> queue;
    std::map<std::uint64_t, std::uint64_t> reservations;
    AsyncLimits limits;
    WriterStatus stats;
    std::array<HANDLE, kMaxWorkers> threads{};
    HANDLE status_thread = nullptr;
    std::vector<std::string> status_directories;
    std::array<const Entry *, kMaxWorkers> active{};
    std::size_t thread_count = 0, live_workers = 0;
    HANDLE cancel_event = nullptr;
    HMODULE module_hold = nullptr;
    std::string event_name;
    std::uint64_t next_id = 0, generation = 0;
    bool stopping = false, cancelled = false, reported = true;

    Impl(Saver saver, MemoryProbe memory) : save(saver), probe(memory ? memory : memory_status)
    {
        if (!save) throw std::runtime_error("后台保存函数缺失");
    }

    void check_accepting(std::uint64_t cycle)
    {
        if (cancel_event && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0 && !cancelled)
        {
            cancelled = true;
            stats.accepting = false;
            ++stats.rejected;
            changed.notify_all();
        }
        if (cycle != generation || !stats.running || !stats.accepting)
            throw std::runtime_error("导出已停止接收，请结束排空后重新开始");
    }

    bool memory_ready(std::uint64_t additional)
    {
        std::uint64_t total = 0, available = 0, commit = 0;
        if (!probe(total, available, commit) || !total || available > total)
            throw std::runtime_error("无法读取有效的进程内存统计，停止新增输入");
        // 地址占用只用于诊断，不再以进程占用百分比阻止申请。
        memory = {total, total - available, commit, additional, true};
        if (additional > commit)
        {
            blocked_by = "commit_limit";
            return false;
        }
        return true;
    }

    bool reserve_ready(std::uint64_t bytes)
    {
        if (stats.unfinished + stats.reserved >= limits.capacity) { blocked_by = "queue_count"; return false; }
        if (bytes > limits.bytes - stats.bytes) { blocked_by = "queue_bytes"; return false; }
        return memory_ready(bytes);
    }

    template<typename Predicate>
    WaitResult wait(std::unique_lock<std::mutex> &lock, Predicate ready, const char *reason)
    {
        const auto cycle = generation;
        const auto begin = ExportClock::now();
        auto next_report = begin;
        WaitResult result;
        bool waiting = false;
        try
        {
            for (;;)
            {
                check_accepting(cycle);
                blocked_by = "unknown";
                memory = {};
                if (ready()) break;
                result.blocked_by = blocked_by;
                if (!waiting)
                {
                    waiting = true;
                    ++stats.waiting;
                }
                const auto now = ExportClock::now();
                if (now >= next_report)
                {
                    const auto snapshot = stats;
                    const auto sampled = memory;
                    const auto capacity = limits.capacity;
                    const auto memory_limit = limits.bytes;
                    const auto stop_name = event_name;
                    const auto cause = blocked_by;
                    const auto waited_ms = std::chrono::duration<double, std::milli>(now - begin).count();
                    next_report = now + std::chrono::seconds(5);
                    lock.unlock();
                    try { LOG_WARN("等待导出资源 reason=%s blocked_by=%s pending=%llu reserved=%llu busy=%llu "
                                   "capacity=%llu bytes=%llu memory_bytes=%llu memory_checked=%d virtual_total=%llu "
                                   "virtual_used=%llu additional=%llu commit_available=%llu "
                                   "waited_ms=%.3f stop_event=%s",
                                   reason, cause, snapshot.unfinished, snapshot.reserved, snapshot.busy,
                                   static_cast<unsigned long long>(capacity), snapshot.bytes, memory_limit,
                                   sampled.checked ? 1 : 0, sampled.total, sampled.used,
                                   sampled.additional, sampled.commit, waited_ms, stop_name.c_str()); }
                    catch (...) {}
                    lock.lock();
                }
                changed.wait_for(lock, std::chrono::milliseconds(200));
            }
        }
        catch (...)
        {
            if (waiting && cycle == generation) --stats.waiting;
            throw;
        }
        if (waiting && cycle == generation) --stats.waiting;
        if (waiting) result.waited_ms = std::chrono::duration<double, std::milli>(ExportClock::now() - begin).count();
        return result;
    }

    static void log_recovered(const WaitResult &result, const char *phase) noexcept
    {
        if (result.waited_ms < 0) return;
        try { LOG_INFO("导出资源恢复 phase=%s blocked_by=%s waited_ms=%.3f，资源检查通过，继续接收或提交",
                       phase, result.blocked_by, result.waited_ms); }
        catch (...) {}
    }

    std::deque<Entry>::iterator next_ready()
    {
        // 跳过正在保存的同文件任务；队列中该文件最早的一项始终先被选中。
        return std::find_if(queue.begin(), queue.end(), [&](const Entry &pending) {
            for (const auto *saving : active)
                if (saving && saving->target == pending.target) return false;
            return true;
        });
    }

    void join_workers()
    {
        // 调用者持有生命周期锁，不持有队列锁。
        for (auto &thread : threads)
        {
            if (!thread) continue;
            if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
                throw std::runtime_error("无法确认全部后台线程已经退出");
            CloseHandle(thread);
            thread = nullptr;
        }
        if (status_thread)
        {
            if (WaitForSingleObject(status_thread, INFINITE) != WAIT_OBJECT_0)
                throw std::runtime_error("无法确认后台状态线程已经退出");
            CloseHandle(status_thread);
            status_thread = nullptr;
        }
    }

    void report_status() noexcept
    {
        auto next_memory_report = ExportClock::time_point{};
        for (;;)
        {
            try
            {
                std::unique_lock<std::mutex> lock(mutex);
                bool initialized = false, was_busy = false;
                std::size_t directory_count = 0;
                std::uint64_t accepted = 0;
                auto next_update = ExportClock::now();
                for (;;)
                {
                    const bool busy = stats.unfinished != 0;
                    const bool stopped = stopping && live_workers == 0;
                    const auto now = ExportClock::now();
                    if (!initialized || directory_count != status_directories.size() ||
                        (busy && (!was_busy || now >= next_update)) ||
                        (!busy && (was_busy || accepted != stats.accepted)) || stopped)
                    {
                        const auto directories = status_directories;
                        const auto name = "_bg_" + std::to_string(queue.size()) + "-" + std::to_string(limits.capacity) +
                            "_" + std::to_string(stats.busy) + "-" + std::to_string(limits.workers);
                        initialized = true;
                        was_busy = busy;
                        accepted = stats.accepted;
                        directory_count = directories.size();
                        next_update = now + std::chrono::seconds(5);
                        // 文件系统操作不持有队列锁，不阻塞前台输入或保存线程领取任务。
                        lock.unlock();
                        for (const auto &directory : directories) update_status_file(directory, name);
                        lock.lock();
                        if (stopped) return;
                        continue;
                    }
                    if (busy) changed.wait_until(lock, next_update);
                    else changed.wait(lock);
                }
            }
            catch (const std::bad_alloc &)
            {
                // 内存压力不能令状态线程永久退出；停机时允许最后发布失败后结束。
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping && live_workers == 0) return;
                }
                const auto now = ExportClock::now();
                if (now >= next_memory_report)
                {
                    next_memory_report = now + std::chrono::seconds(5);
                    try { LOG_WARN("%s", "后台状态内存不足 retry_ms=200，保留线程并重试"); } catch (...) {}
                }
                Sleep(200);
            }
            catch (...)
            {
                try { LOG_ERROR("后台状态文件更新失败：状态线程异常退出，CSV 保存继续"); } catch (...) {}
                return;
            }
        }
    }

    static unsigned __stdcall status_entry(void *context) noexcept
    {
        static_cast<Impl *>(context)->report_status();
        return 0;
    }

    void run() noexcept
    {
        try
        {
            for (;;)
            {
                Entry entry;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return (stopping && queue.empty()) || next_ready() != queue.end(); });
                    if (queue.empty()) break;
                    const auto next = next_ready();
                    entry = std::move(*next);
                    queue.erase(next);
                    *std::find(active.begin(), active.end(), nullptr) = &entry;
                    ++stats.busy;
                }
                bool ok = false;
                try { ok = save(*entry.job); }
                catch (...)
                {
                    try { LOG_ERROR("后台保存异常 id=%llu symbol=%s period=%s，请修正后重新导出",
                                    entry.job->id, entry.job->symbol.c_str(), entry.job->period.c_str()); }
                    catch (...) {}
                }
                entry.job.reset(); // 真正释放数据后，才让前台复用额度。
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    --stats.unfinished;
                    stats.bytes -= entry.bytes;
                    *std::find(active.begin(), active.end(), &entry) = nullptr;
                    --stats.busy;
                    if (ok) ++stats.succeeded;
                    else ++stats.failed;
                }
                changed.notify_all();
            }
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats.failed;
            stats.accepting = false;
            stopping = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stats.running = --live_workers != 0;
        }
        changed.notify_all();
    }

    static unsigned __stdcall entry(void *context) noexcept
    {
        static_cast<Impl *>(context)->run();
        return 0;
    }
};

AsyncWriter::AsyncWriter(Saver save, MemoryProbe probe) : impl_(new Impl(save, probe)) {}
AsyncWriter::~AsyncWriter()
{
    // 正常动态卸载时线程已显式结束；避免在 DllMain 中再次加锁或等待。
    if (impl_->thread_count) { try { finish(); } catch (...) {} }
}

void AsyncWriter::start(AsyncLimits limits, const std::string &csv_dir)
{
    if (!limits.capacity || !limits.bytes) throw std::runtime_error("队列数量和内存上限必须为正整数");
    if (!limits.workers || limits.workers > kMaxWorkers) throw std::runtime_error("后台保存线程数必须在 1 到 128 之间");
    auto &p = *impl_;
    std::lock_guard<std::mutex> lifecycle(p.lifecycle);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.thread_count)
        {
            if (!p.stats.accepting || !p.stats.running)
                throw std::runtime_error("后台尚未结束，请先执行排空停机");
            if (limits.capacity != p.limits.capacity || limits.bytes != p.limits.bytes || limits.workers != p.limits.workers)
                throw std::runtime_error("队列配置已变化，请先排空停机后重启导出");
            if (!csv_dir.empty() && std::find(p.status_directories.begin(), p.status_directories.end(), csv_dir) == p.status_directories.end())
            {
                p.status_directories.push_back(csv_dir);
                p.changed.notify_all();
            }
            return;
        }
        if (p.stats.unfinished) throw std::runtime_error("后台故障仍有未完成任务，不能重置结果");
    }
    std::vector<std::string> directories;
    if (!csv_dir.empty()) directories.push_back(csv_dir);
    // 持有引用直到显式停机；禁止未停机时强行卸载 DLL。
    HMODULE hold = nullptr;
    if (export_module && !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                            reinterpret_cast<LPCSTR>(export_module), &hold))
        throw std::runtime_error("无法保持 DLL 模块引用，未启动后台线程");
    char name[128];
    sprintf_s(name, "Local\\SaveAsCSV_StopInput_%lu_%p_%llu_%llu", GetCurrentProcessId(), &p,
              p.generation + 1, static_cast<unsigned long long>(ExportClock::now().time_since_epoch().count()));
    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, name);
    if (!event)
    {
        if (hold) FreeLibrary(hold);
        throw std::runtime_error("无法建立停止接收事件");
    }
    bool start_failed = false;
    try
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.event_name = name;
        const auto unreported = p.reported ? 0 : p.stats.rejected;
        p.stats = WriterStatus{};
        p.stats.rejected = unreported;
        p.reported = false;
        p.limits = limits;
        p.status_directories = std::move(directories);
        p.stopping = p.cancelled = false;
        ++p.generation;
        p.cancel_event = event;
        p.module_hold = hold;
        p.stats.accepting = true;
        for (std::size_t i = 0; i < limits.workers; ++i)
        {
            auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Impl::entry, &p, 0, nullptr));
            if (!thread)
            {
                p.stats.accepting = false;
                p.stopping = start_failed = true;
                break;
            }
            p.threads[p.thread_count++] = thread;
            ++p.live_workers;
        }
        p.stats.running = p.live_workers != 0;
        if (!start_failed)
        {
            p.status_thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Impl::status_entry, &p, 0, nullptr));
            if (!p.status_thread)
            {
                p.stats.accepting = false;
                p.stopping = start_failed = true;
            }
        }
    }
    catch (...)
    {
        CloseHandle(event);
        if (hold) FreeLibrary(hold);
        throw;
    }
    if (start_failed)
    {
        p.changed.notify_all();
        p.join_workers();
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            p.thread_count = 0;
            p.cancel_event = nullptr;
            p.module_hold = nullptr;
        }
        CloseHandle(event);
        if (hold) FreeLibrary(hold);
        throw std::runtime_error("无法启动全部后台保存和状态线程，已停止并回收本次已启动线程");
    }
    try { LOG_INFO("后台启动 capacity=%llu memory_bytes=%llu worker_threads=%llu stop_event=%s",
                   static_cast<unsigned long long>(limits.capacity), limits.bytes,
                   static_cast<unsigned long long>(limits.workers), p.event_name.c_str()); }
    catch (...) {}
}

std::uint64_t AsyncWriter::reserve(std::uint64_t bytes)
{
    auto &p = *impl_;
    std::unique_lock<std::mutex> lock(p.mutex);
    if (!bytes || bytes > p.limits.bytes) throw std::runtime_error("单个任务超过队列内存上限");
    const auto waited = p.wait(lock, [&] {
        return p.reserve_ready(bytes);
    }, "接收前数量/字节/系统提交额度");
    if (p.next_id == UINT64_MAX) throw std::runtime_error("任务序号耗尽，请重启程序");
    const auto id = ++p.next_id;
    p.reservations.emplace(id, bytes);
    ++p.stats.reserved;
    p.stats.bytes += bytes;
    lock.unlock();
    Impl::log_recovered(waited, "reserve");
    return id;
}

std::uint64_t AsyncWriter::try_reserve(std::uint64_t bytes)
{
    auto &p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    p.check_accepting(p.generation);
    if (!bytes || bytes > p.limits.bytes) throw std::runtime_error("单个任务超过队列内存上限");
    if (!p.reserve_ready(bytes)) return 0;
    if (p.next_id == UINT64_MAX) throw std::runtime_error("任务序号耗尽，请重启程序");
    const auto id = ++p.next_id;
    p.reservations.emplace(id, bytes);
    ++p.stats.reserved;
    p.stats.bytes += bytes;
    return id;
}

void AsyncWriter::check_memory(std::uint64_t additional)
{
    auto &p = *impl_;
    std::unique_lock<std::mutex> lock(p.mutex);
    const auto waited = p.wait(lock, [&] { return p.memory_ready(additional); }, "实际申请前系统提交额度");
    lock.unlock();
    Impl::log_recovered(waited, "allocate");
}

void AsyncWriter::wait_allocation()
{
    auto &p = *impl_;
    std::unique_lock<std::mutex> lock(p.mutex);
    const auto cycle = p.generation;
    p.check_accepting(cycle);
    ++p.stats.waiting;
    try
    {
        p.changed.wait_for(lock, std::chrono::milliseconds(200), [&] {
            return cycle != p.generation || !p.stats.accepting;
        });
    }
    catch (...)
    {
        if (cycle == p.generation) --p.stats.waiting;
        throw;
    }
    if (cycle == p.generation) --p.stats.waiting;
    p.check_accepting(cycle);
}

void AsyncWriter::release(std::uint64_t reservation) noexcept
{
    auto &p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    auto found = p.reservations.find(reservation);
    if (found == p.reservations.end()) return;
    p.stats.bytes -= found->second;
    --p.stats.reserved;
    p.reservations.erase(found);
    p.changed.notify_all();
}

void AsyncWriter::submit(std::uint64_t reservation, ExportJob &&job)
{
    const auto begin = ExportClock::now();
    auto &p = *impl_;
    const auto bytes = job_memory(job);
    std::string target = job.csv_dir;
    while (!target.empty() && (target.back() == '\\' || target.back() == '/')) target.pop_back();
    target += "\\" + job.symbol + "." + job.period + ".csv";
    for (auto &c : target)
    {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    std::unique_lock<std::mutex> lock(p.mutex);
    if (bytes > p.limits.bytes) throw std::runtime_error("实际任务超过队列内存上限");
    const auto waited = p.wait(lock, [&] {
        auto found = p.reservations.find(reservation);
        if (found == p.reservations.end()) throw std::runtime_error("采集额度已经失效，不能提交");
        const auto old = found->second;
        if (bytes > p.limits.bytes - (p.stats.bytes - old))
        {
            p.blocked_by = "queue_bytes";
            return false;
        }
        return p.memory_ready(0);
    }, "提交前实际字节/系统提交额度");
    auto found = p.reservations.find(reservation);
    p.queue.emplace_back();
    try { p.queue.back().job.reset(new ExportJob(std::move(job))); }
    catch (...) { p.queue.pop_back(); throw; }
    auto &entry = p.queue.back();
    entry.target = std::move(target);
    entry.bytes = bytes;
    entry.job->id = reservation;
    entry.job->queued = ExportClock::now();
    entry.job->call_ms[9] = std::chrono::duration<double, std::milli>(entry.job->queued - begin).count();
    p.stats.bytes = p.stats.bytes - found->second + bytes;
    p.reservations.erase(found);
    --p.stats.reserved;
    ++p.stats.unfinished;
    ++p.stats.accepted;
    p.changed.notify_all();
    lock.unlock();
    Impl::log_recovered(waited, "submit");
}

void AsyncWriter::note_rejected() noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->stats.rejected;
    impl_->reported = false;
}

WriterStatus AsyncWriter::status()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

void AsyncWriter::cancel_input() noexcept
{
    auto &p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.cancelled && p.stats.accepting)
    {
        p.cancelled = true;
        p.stats.accepting = false;
        ++p.stats.rejected;
        if (p.cancel_event) SetEvent(p.cancel_event);
    }
    p.changed.notify_all();
}

void AsyncWriter::stop_input() noexcept
{
    auto &p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    p.stats.accepting = false;
    p.cancelled = true;
    p.changed.notify_all();
}

std::string AsyncWriter::cancel_event_name()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->event_name;
}

bool AsyncWriter::finish()
{
    auto &p = *impl_;
    std::lock_guard<std::mutex> lifecycle(p.lifecycle);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.stats.accepting = false;
        p.stopping = true;
        for (const auto &entry : p.reservations) p.stats.bytes -= entry.second;
        p.reservations.clear();
        p.stats.reserved = 0;
    }
    p.changed.notify_all();
    p.join_workers();
    HMODULE hold;
    bool ok;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.thread_count = 0;
        if (p.cancel_event) CloseHandle(p.cancel_event);
        p.cancel_event = nullptr;
        hold = p.module_hold;
        p.module_hold = nullptr;
        ok = !p.stats.failed && !p.stats.rejected && !p.stats.unfinished;
        p.reported = true;
    }
    // 正常调用方仍须持有 DLL 绑定引用；不能通过已失效的裸函数指针停机。
    if (hold) FreeLibrary(hold);
    return ok;
}
} // namespace tdx_export
