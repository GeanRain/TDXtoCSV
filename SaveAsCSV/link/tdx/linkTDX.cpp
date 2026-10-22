/****************************************************************************
 * SaveAsCSV TDX Export DLL
 * Copyright (C) 2026, GeanRain
 ****************************************************************************/
#include "linkTDX.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../common.h"
#include "../../export/async_writer.h"
#ifndef TDX_EXPORT_INPROCESS_TEST
#include "../../export/process_writer.h"
#endif
#include "../../../shared/csv_store.h"
#include "../../utils/ini_reader.h"
#include "../../utils/logger.h"

extern std::string config_file_dir;
#ifndef TDX_EXPORT_INPROCESS_TEST
extern HANDLE backend_job;
extern std::uint64_t backend_lifetime;
extern bool reset_backend_lifetime() noexcept;
#endif

namespace
{
using namespace tdx_export;

// 后台只接收 ExportJob；调用链标记仍由前台线程单独持有。
struct ExportState : ExportJob
{
    bool active = false;
    bool failed = false;
    int stage = 0;
    std::uint64_t reservation = 0;
    std::uint64_t lifetime = 0;
    std::string worker_path;
};

ExportState &state()
{
    // 同一线程先完成一个品种的全部调用，再开始下一品种。
    static thread_local ExportState current;
#ifndef TDX_EXPORT_INPROCESS_TEST
    if (current.lifetime != backend_lifetime)
    {
        // 重新注册时旧后台已经结束，旧线程暂存和预留不能混入新后台。
        current = ExportState{};
        current.lifetime = backend_lifetime;
    }
#endif
    return current;
}

#ifdef TDX_EXPORT_INPROCESS_TEST
using ExportWriter = tdx_export::AsyncWriter;
#else
using ExportWriter = tdx_export::ProcessWriter;
#endif
ExportWriter *export_writer = nullptr;
SRWLOCK writer_init_lock = SRWLOCK_INIT;

ExportWriter &writer()
{
    AcquireSRWLockExclusive(&writer_init_lock);
    try
    {
        if (!export_writer)
#ifdef TDX_EXPORT_INPROCESS_TEST
            export_writer = new ExportWriter(perform_save);
#else
            export_writer = new ExportWriter(config_file_dir, backend_job, backend_lifetime);
#endif
    }
    catch (...) { ReleaseSRWLockExclusive(&writer_init_lock); throw; }
    ReleaseSRWLockExclusive(&writer_init_lock);
    return *export_writer;
}

void release_current() noexcept
{
    auto &s = state();
    // 先释放实际数组，再归还接收额度。
    std::vector<float>().swap(s.dates);
    std::vector<float>().swap(s.times);
    for (auto &values : s.series) std::vector<float>().swap(values);
    if (s.reservation && export_writer) export_writer->release(s.reservation);
    s.reservation = 0;
}

void fail_current() noexcept
{
    auto &s = state();
    if (!s.failed)
    {
        s.failed = true;
        try { writer().note_rejected(); } catch (...) {}
    }
    release_current();
}

float reject(const char *message, bool invalidate = true)
{
    if (invalidate)
        fail_current();
    LOG_ERROR("%s", message);
    return -1.0f;
}

template<typename Action>
auto retry_allocation(Action action, const char *phase, bool input, const ExportJob *job = nullptr) -> decltype(action())
{
    const auto begin = ExportClock::now();
    auto next_report = begin;
    std::uint64_t retries = 0;
    for (;;)
    {
        try
        {
            auto result = action();
            if (retries)
                try { LOG_INFO("内存分配恢复 phase=%s id=%llu retries=%llu waited_ms=%.3f",
                               phase, job ? job->id : 0, retries, elapsed_ms(begin)); } catch (...) {}
            return result;
        }
        catch (const std::bad_alloc &)
        {
            ++retries;
            const auto now = ExportClock::now();
            if (now >= next_report)
            {
                next_report = now + std::chrono::seconds(5);
                try { LOG_WARN("内存分配重试 phase=%s id=%llu retries=%llu retry_ms=200 waited_ms=%.3f",
                               phase, job ? job->id : 0, retries, elapsed_ms(begin)); } catch (...) {}
            }
            // 退出 action 后，其临时对象和文件锁已销毁；等待时不持有队列锁。
            if (input && export_writer && export_writer->status().running) export_writer->wait_allocation();
            else Sleep(200);
        }
    }
}

template<typename Action>
float execute(int count, float *output, Action action, int timing_slot = -1, bool invalidate = true) noexcept
{
    const auto begin = ExportClock::now();
    float result = -1.0f;
    try
    {
        if (count > 0 && count <= kMaxCount)
            result = retry_allocation(action, "receive", true);
        else
            reject("数据条数必须在 1 到 2000000 之间", invalidate);
    }
    catch (const std::exception &error)
    {
        if (invalidate) fail_current();
        try { LOG_ERROR("导出调用失败：%s", error.what()); } catch (...) {}
    }
    catch (...)
    {
        if (invalidate) fail_current();
        try { LOG_ERROR("%s", "导出发生异常，本次失败，原文件不应被覆盖"); }
        catch (...) {}
    }
    if (output && count > 0)
        std::fill(output, output + count, result);
    if (timing_slot >= 0)
    {
        auto &s = state();
        if (timing_slot == 0 && result == 1.0f) s.started = begin;
        s.call_ms[timing_slot] += elapsed_ms(begin);
    }
    return result;
}

bool check_export(int count, const float *previous_result)
{
    auto &s = state();
    if (!s.active)
    {
        reject("本次导出未开始或已结束，请从开始步骤重跑");
        return false;
    }
    if (s.failed)
    {
        reject("本次导出已失败，请从开始步骤重跑");
        return false;
    }
    if (!previous_result || previous_result[0] != 1.0f)
    {
        reject("上一步未成功，请从开始步骤重跑完整公式");
        return false;
    }
    if (count != s.count)
    {
        reject("本次导出的数组长度不一致");
        return false;
    }
    return true;
}

std::string market_suffix(int code)
{
    switch (code)
    {
    case 0: return "SZ";
    case 1: return "SH";
    case 2: return "BJ";
    case 47: return "CFFEX";
    case 28: return "CZCE";
    case 29: return "DCE";
    case 30: return "SHFE";
    case 27: return "HKI";
    case 31: return "HK";
    case 48: return "HKGEM";
    default: return "";
    }
}

int config_integer(IniReader &ini, const char *name, const char *fallback, const char *section = "export")
{
    std::string text = ini.getString(section, name, fallback);
    if (text.empty() || text.size() > 8) throw std::runtime_error("配置整数无效");
    int result = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9') throw std::runtime_error("配置整数无效");
        result = result * 10 + c - '0';
    }
    return result;
}

struct ExportConfig
{
    int default_time = 150000, width = 6;
    std::string csv_dir, worker_path;
    AsyncLimits limits;
};

ExportConfig load_config()
{
    ExportConfig result;
    std::string ini_path = join_path(config_file_dir, INIREADER_FILENAME);
    IniReader ini(ini_path, kDefaultIni);
    std::ifstream config;
    config.exceptions(std::ios::badbit);
    config.open(ini_path, std::ios::binary);
    if (!ini.isValid() || !config.is_open()) throw std::runtime_error("配置文件无法读取");
    result.default_time = config_integer(ini, "default_time", "150000");
    result.width = config_integer(ini, "symbol_width", "6");
    if (!valid_time(result.default_time) || result.width < 1 || result.width > 12)
        throw std::runtime_error("时间或代码位数配置无效");
    std::string base = ini.getString("export", "csv_dir", "");
    if (base.size() >= 255) throw std::runtime_error("输出路径过长");
    if (base.empty()) base = join_path(config_file_dir, "csv");
    else if (!(base.size() >= 2 && (base[1] == ':' || base.substr(0, 2) == "\\\\")))
        base = join_path(config_file_dir, base);
    if (base.size() >= 2 && base[1] == ':' &&
        (base.size() < 3 || (base[2] != '\\' && base[2] != '/')))
        throw std::runtime_error("输出路径不能使用驱动器相对路径");
    char absolute[MAX_PATH];
    DWORD size = GetFullPathNameA(base.c_str(), MAX_PATH, absolute, nullptr);
    if (!size || size >= MAX_PATH) throw std::runtime_error("输出路径无效或过长");
    result.csv_dir = absolute;
    int capacity = config_integer(ini, "queue_capacity", "1000", "async");
    int megabytes = config_integer(ini, "queue_memory_mb", "2048", "async");
    int workers = config_integer(ini, "worker_threads", "8", "async");
    result.worker_path = ini.getString("async", "worker_path", "SaveAsCSVWorker.exe");
    if (capacity <= 0 || megabytes <= 0) throw std::runtime_error("异步队列数量和内存上限必须大于零");
    if (workers < 1 || workers > static_cast<int>(tdx_export::kMaxWorkers)) throw std::runtime_error("后台保存线程数必须在 1 到 128 之间");
    result.limits = {static_cast<std::size_t>(capacity), static_cast<std::uint64_t>(megabytes) * 1024 * 1024,
                     static_cast<std::size_t>(workers)};
    return result;
}

} // namespace

void tdx_export::dispose_export_writer() noexcept
{
    // DllMain 已关闭作业句柄结束后台；此处只释放代理，不进行管道调用或等待。
    delete export_writer;
    export_writer = nullptr;
}

// 4：清空上次状态，一次设置本次品种与原始周期；成功返回 1。
void Func4(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3)
{
    static std::atomic<bool> entered{false};
    const bool first_call = !entered.exchange(true);
    if (first_call) try { LOG_INFO("通达信首次进入导出接口 nCount=%d thread=%lu", nCount, GetCurrentThreadId()); } catch (...) {}
    execute(nCount, pOut, [&]() -> float
    {
        auto &s = state();
        release_current();
        s = ExportState{};
#ifndef TDX_EXPORT_INPROCESS_TEST
        s.lifetime = backend_lifetime;
#endif
        int code = 0, market = 0, period = 0;
        if (!pIn1 || !pIn2 || !pIn3 || !integer(pIn1[0], 0, 999999, code) ||
            !integer(pIn2[0], 0, 65535, market) || market_suffix(market).empty() ||
            !integer(pIn3[0], 0, 65535, period))
            return reject("代码、市场或周期输入无效");
        s.count = nCount;
        s.native_period = period;
        const auto config = load_config();
        s.default_time = config.default_time;
        s.csv_dir = config.csv_dir;
        s.worker_path = config.worker_path;
        std::ostringstream symbol;
        symbol.exceptions(std::ios::badbit);
        symbol.imbue(std::locale::classic());
        symbol << std::setfill('0') << std::setw(config.width) << code;
        s.symbol = symbol.str();
        if (first_call) try { LOG_INFO("首次导出配置读取完成 worker_path=%s csv_dir=%s", s.worker_path.c_str(), s.csv_dir.c_str()); } catch (...) {}
        s.symbol += "." + market_suffix(market);
        s.caller_thread = GetCurrentThreadId();
#ifdef TDX_EXPORT_INPROCESS_TEST
        writer().start(config.limits, s.csv_dir);
#else
        writer().start(config.limits, s.csv_dir, s.worker_path);
#endif
        s.reservation = writer().reserve(tdx_export::job_memory(s) + static_cast<std::uint64_t>(nCount) * 32);
        // 完整接收前先分配数组，后续 assign/resize 不再重复申请整份空间。
        writer().check_memory(static_cast<std::uint64_t>(nCount) * sizeof(float));
        s.dates.reserve(nCount);
        writer().check_memory(static_cast<std::uint64_t>(nCount) * sizeof(float));
        s.times.reserve(nCount);
        for (auto &values : s.series)
        {
            writer().check_memory(static_cast<std::uint64_t>(nCount) * sizeof(float));
            values.reserve(nCount);
        }
        s.active = true;
        return 1.0f;
    }, 0);
}

// 5：明确不复权口径；第三个参数保留，必须为零。
void Func5(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3)
{
    execute(nCount, pOut, [&]() -> float
    {
        if (!check_export(nCount, pIn1)) return -1.0f;
        auto &s = state();
        int adjustment = 0, reserved = 0;
        if (s.stage != 0 || !pIn2 || !pIn3 || !integer(pIn2[0], 0, 2, adjustment) ||
            adjustment != 0 || !integer(pIn3[0], 0, 0, reserved))
            return reject("设置顺序错误、不是不复权数据，或保留参数不为零");
        s.period = period_name(s.native_period);
        if (s.period.empty())
            return reject("不支持该周期，仅支持 1/5/15/30/60 分钟及日、周、月、年");
        s.minute = s.native_period <= 4;
        s.stage = 1;
        return 1.0f;
    }, 1);
}

// 2：原样复制日期时间；内容校验在后台，第三个参数为上一步结果。
void Func2(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3)
{
    execute(nCount, pOut, [&]() -> float
    {
        if (!check_export(nCount, pIn3)) return -1.0f;
        auto &s = state();
        if (s.stage != 1 || !pIn1 || (s.minute && !pIn2))
            return reject("日期时间缺失或调用顺序错误");
        s.dates.assign(pIn1, pIn1 + nCount);
        if (s.minute) s.times.assign(pIn2, pIn2 + nCount);
        else s.times.clear();
        s.stage = 2;
        return 1.0f;
    }, 2);
}

// 3：按 1 开、2 收、3 高、4 低、5 量、6 额各复制一次，内容校验由后台完成。
void Func3(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3)
{
    int type = 0;
    if (nCount > 0 && nCount <= kMaxCount && pIn1)
        integer(pIn1[0], 1, 6, type);
    execute(nCount, pOut, [&]() -> float
    {
        if (!check_export(nCount, pIn3)) return -1.0f;
        auto &s = state();
        if (type == 0 || !pIn2 || s.stage != type + 1)
            return reject("数值数组缺失，或未按 1 至 6 顺序传入");
        s.series[type - 1].assign(pIn2, pIn2 + nCount);
        ++s.stage;
        return 1.0f;
    }, type > 0 ? type + 2 : -1);
}

// 1：只接受命令 3；完整数据移交后台，返回 1 仅表示提交成功。
void Func1(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3)
{
    (void)pIn3;
    auto &s = state();
    std::fill(s.save_ms, s.save_ms + 4, 0.0);
    s.call_ms[9] = 0;
    const auto begin = ExportClock::now();
    std::string symbol, period;
    std::uint64_t id = 0;
    const float result = execute(nCount, pOut, [&]() -> float
    {
        int command = 0;
        if (!pIn1 || !integer(pIn1[0], 3, 3, command))
            return reject("请更新公式：使用 4 号开始、5 号确认，并依次传递上一步结果", false);
        if (!check_export(nCount, pIn2)) return -1.0f;
        if (s.stage != 8) return reject("本次数据未完整接收");
        symbol = s.symbol;
        period = s.period;
        id = s.reservation;
        writer().submit(s.reservation, std::move(static_cast<ExportJob &>(s)));
        s.reservation = 0;
        s.active = false;
        return 1.0f;
    }, 9);
    if (result == 1.0f)
        try { LOG_INFO("提交成功 id=%lu:%llu symbol=%s period=%s input=%d submit_call_ms=%.3f，后台结果另行记录",
                       GetCurrentThreadId(), id, symbol.c_str(), period.c_str(), nCount, elapsed_ms(begin)); }
        catch (...) {}
}

bool control_arguments(float *a, float *b, float *c, int max_command, int &command)
{
    return a && b && c && integer(a[0], 0, max_command, command) && b[0] == 0 && c[0] == 0;
}

// 6：0 未完成、1 排队、2 正在保存、3 失败标志、4 任务MiB、5 采集预留、6 等待者。
void Func6(int nCount, float *pOut, float *a, float *b, float *c)
{
    execute(nCount, pOut, [&]() -> float {
        int command = 0;
        if (!control_arguments(a, b, c, 6, command)) return reject("状态查询参数无效", false);
        const auto value = writer().status();
        switch (command)
        {
        case 0: return static_cast<float>(value.unfinished);
        case 1: return static_cast<float>(value.unfinished - value.busy);
        case 2: return static_cast<float>(value.busy);
        case 3: return value.failed || value.rejected || (!value.running && value.unfinished) ? 1.0f : 0.0f;
        case 4: return static_cast<float>(value.bytes / (1024.0 * 1024.0));
        case 5: return static_cast<float>(value.reserved);
        default: return static_cast<float>(value.waiting);
        }
    }, -1, false);
}

// 7：停止接收、等待后台处理结束并停机；本轮有任一失败返回 -1。
void Func7(int nCount, float *pOut, float *a, float *b, float *c)
{
    execute(nCount, pOut, [&]() -> float {
        int command = 0;
        if (!control_arguments(a, b, c, 0, command)) return reject("排空参数必须全部为零", false);
        if (state().active && !state().failed)
        {
            fail_current();
            try { LOG_ERROR("排空时仍有未提交采集 symbol=%s period=%s，本次中止，请重新导出",
                            state().symbol.c_str(), state().period.c_str()); } catch (...) {}
        }
        release_current();
        state().active = false;
        const bool ok = writer().finish();
        const auto value = writer().status();
        try { LOG_INFO("排空完成 accepted=%llu succeeded=%llu failed=%llu rejected=%llu remaining=%llu result=%s",
                       value.accepted, value.succeeded, value.failed, value.rejected,
                       value.unfinished, ok ? "ok" : "failed"); } catch (...) {}
        return ok ? 1.0f : -1.0f;
    }, -1, false);
}

// 8：停止新增输入并唤醒等待者；已提交任务继续保存，随后仍需调用 7 排空。
void Func8(int nCount, float *pOut, float *a, float *b, float *c)
{
    execute(nCount, pOut, [&]() -> float {
        int command = 0;
        if (!control_arguments(a, b, c, 0, command)) return reject("停止接收参数必须全部为零", false);
        writer().cancel_input();
        return 1.0f;
    }, -1, false);
}

static PluginTCalcFuncInfo Info[] =
{
    {1, &Func1}, {2, &Func2}, {3, &Func3}, {4, &Func4},
    {5, &Func5}, {6, &Func6}, {7, &Func7}, {8, &Func8}, {0, nullptr}
};

BOOL RegisterTdxFunc(PluginTCalcFuncInfo **pInfo)
{
    if (!pInfo || *pInfo) return FALSE;
#ifndef TDX_EXPORT_INPROCESS_TEST
    // 注册发生在 DllMain 返回后；不能在持有装卸锁时启动进程或等待管道。
    try
    {
        if (export_writer)
        {
            // 兼容宿主仅重新注册、未真正 FreeLibrary 的重新绑定方式。
            const bool ready = reset_backend_lifetime();
            tdx_export::dispose_export_writer();
            if (!ready) throw std::runtime_error("无法重新建立后台生命周期");
        }
        const auto config = load_config();
        writer().start(config.limits, config.csv_dir, config.worker_path);
    }
    catch (const std::exception &error)
    {
        try { LOG_ERROR("DLL绑定时启动后台失败：%s", error.what()); } catch (...) {}
        return FALSE;
    }
    catch (...)
    {
        try { LOG_ERROR("%s", "DLL绑定时启动后台发生异常"); } catch (...) {}
        return FALSE;
    }
#endif
    *pInfo = Info;
    try { LOG_INFO("DLL接口注册成功 functions=8 pid=%lu", GetCurrentProcessId()); } catch (...) {}
    return TRUE;
}
