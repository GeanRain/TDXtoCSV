// 保存流程回归：使用生产实现和真实文件，捕获日志；大小上限使用显式故障注入。
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
namespace allocation_fault {
thread_local bool fail_next=false;
thread_local int probe_calls=0;
thread_local int fail_after=-1, allocations=0;
std::atomic<int> triggered{0}, probe_failures{0}, stride{1}, temp_failures{0};
}
void *operator new(std::size_t bytes) {
    ++allocation_fault::allocations;
    if(allocation_fault::fail_next||allocation_fault::fail_after==0) {
        allocation_fault::fail_next=false;allocation_fault::fail_after=-1;++allocation_fault::triggered;throw std::bad_alloc();
    }
    if(allocation_fault::fail_after>0)--allocation_fault::fail_after;
    if(void *memory=std::malloc(bytes?bytes:1))return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void *memory) noexcept {std::free(memory);}
void operator delete[](void *memory) noexcept {std::free(memory);}
void operator delete(void *memory,std::size_t) noexcept {std::free(memory);}
void operator delete[](void *memory,std::size_t) noexcept {std::free(memory);}
UINT WINAPI fault_temp_file(LPCSTR directory,LPCSTR prefix,UINT unique,LPSTR name) {
    const auto result=GetTempFileNameA(directory,prefix,unique,name);
    if(result&&allocation_fault::temp_failures.load()>0) {
        --allocation_fault::temp_failures;allocation_fault::fail_next=true;
        // 在文件刚创建的边界制造真实 new 失败，不靠耗尽进程内存。
        void *memory=::operator new(64);::operator delete(memory);
    }
    return result;
}
// 仅注入文件大小报告以覆盖提交限额，不创建数百 MiB 的测试文件。
std::atomic<bool> simulate_oversized_file{false};
BOOL WINAPI reported_file_size(HANDLE file, LARGE_INTEGER *size) {
    BOOL ok=GetFileSizeEx(file,size);
    if(ok&&simulate_oversized_file.load()) size->QuadPart=512LL*1024*1024+1;
    return ok;
}
#define GetFileSizeEx reported_file_size
#define GetTempFileNameA fault_temp_file
#include "../shared/csv_store.cpp"
#define TDX_EXPORT_INPROCESS_TEST
#include "../SaveAsCSV/link/tdx/linkTDX.cpp"
#undef TDX_EXPORT_INPROCESS_TEST
using namespace tdx_export;
#undef GetTempFileNameA
#undef GetFileSizeEx

std::string config_file_dir;
const char *kDefaultIni = "[log]\nmode=disable\n[export]\ncsv_dir=csv\nsymbol_width=6\ndefault_time=150000\n";
namespace captured_log {
struct Entry { std::thread::id thread; std::string level; std::string message; };
std::mutex mutex;
std::condition_variable changed;
bool hold_saved=false;
int held=0;
int save_permits=0;
std::vector<Entry> entries;
std::string throw_prefix;
int thrown = 0;
}
void Logger::log(const char *level, const char *format, ...) {
    char buffer[8192]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    std::unique_lock<std::mutex> lock(captured_log::mutex);
    captured_log::entries.push_back({std::this_thread::get_id(), level, buffer});
    if(captured_log::hold_saved&&std::string(buffer).rfind("保存成功：",0)==0) {
        ++captured_log::held;captured_log::changed.notify_all();
        captured_log::changed.wait(lock,[]{return !captured_log::hold_saved||captured_log::save_permits>0;});
        if(captured_log::hold_saved)--captured_log::save_permits;
    }
    if (!captured_log::throw_prefix.empty() &&
        captured_log::entries.back().message.rfind(captured_log::throw_prefix, 0) == 0) {
        ++captured_log::thrown;
        throw std::runtime_error("测试注入：性能日志输出失败");
    }
}

namespace test {
namespace fs = std::filesystem;
using Series = std::vector<float>;
using Function = void (*)(int, float *, float *, float *, float *);
fs::path root;
int passed = 0, failed = 0, serial = 0;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
Series values(int n, float value) { return Series(n, value); }
Series call(Function fn, int n, Series a, Series b, Series c) {
    Series out(n > 0 ? n : 1, -999);
    fn(n, out.data(), a.empty() ? nullptr : a.data(), b.empty() ? nullptr : b.data(), c.empty() ? nullptr : c.data());
    return out;
}
void status(const Series &result, float expected, const std::string &stage) {
    for (float value : result) check(value == expected, stage + " 返回值应为 " + std::to_string(expected) + "，实际 " + std::to_string(value));
}
std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    check(in.is_open(), "无法读取样例 " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void write(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    check(out.good(), "无法写入样例 " + path.string());
}
struct Fixture {
    fs::path dir;
    explicit Fixture(const std::string &extra = "") {
        call(Func7,1,{0},{0},{0}); // 结束并报告上个用例，避免异步状态跨用例残留。
        dir = root / ("case_" + std::to_string(++serial));
        fs::create_directories(dir / "csv");
        config_file_dir = dir.string();
        write(dir / "saveAsCVS.ini", "[log]\nmode=disable\n[export]\ncsv_dir=csv\nsymbol_width=6\ndefault_time=150000\n" + extra);
    }
    ~Fixture() {call(Func7,1,{0},{0},{0});}
    fs::path file(const std::string &period = "1d", const std::string &symbol = "600000.SH") const { return dir / "csv" / (symbol + "." + period + ".csv"); }
};
struct FileHandle {
    HANDLE value;
    explicit FileHandle(HANDLE handle) : value(handle) { check(value != INVALID_HANDLE_VALUE, "无法创建真实文件锁"); }
    ~FileHandle() { CloseHandle(value); }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;
};
struct Batch {
    int n;
    Series previous;
    Batch(int count = 1, int period = 5, int reservedInput = 0, int code = 600000, int market = 1) : n(count) {
        previous = call(Func4, n, values(n, static_cast<float>(code)), values(n, static_cast<float>(market)), values(n, static_cast<float>(period)));
        status(previous, 1, "开始导出");
        previous = call(Func5, n, previous, values(n, 0), values(n, static_cast<float>(reservedInput)));
        status(previous, 1, "设置周期与不复权");
    }
    Series dependency(int count) const {
        // 长度错误用例也提供足够的缓冲；正常流程直接传上一步完整返回数组。
        auto result = previous;
        result.resize(count, previous.front());
        return result;
    }
    Series dates(const Series &dates, const Series &times = {}) { previous = call(Func2, n, dates, times, dependency(n)); return previous; }
    Series field(int type, const Series &data, int count = -1) {
        if (count < 0) count = n;
        previous = call(Func3, count, values(count, static_cast<float>(type)), data, dependency(count));
        return previous;
    }
    void data() {
        for (int type = 1; type <= 6; ++type) {
            float v[] = {0, 10, 11, 12, 9, 100, 1000};
            status(field(type, values(n, v[type])), 1, "传入字段 " + std::to_string(type));
        }
    }
    Series save() {
        previous = call(Func1, n, values(n, 3), dependency(n), values(n, 0));
        if(previous[0]==1) previous=call(Func7,n,values(n,0),values(n,0),values(n,0));
        return previous;
    }
    void complete(const Series &date = {1261016}, const Series &time = {}) {
        status(dates(date, time), 1, "传入日期时间"); data(); status(save(), 1, "保存");
    }
};
size_t lines(const std::string &text) { return static_cast<size_t>(std::count(text.begin(), text.end(), '\n')); }
void contains(const std::string &text, const std::string &part) { check(text.find(part) != std::string::npos, "输出缺少：" + part); }
void reset_logs(const std::string &throwPrefix = "") {
    std::lock_guard<std::mutex> lock(captured_log::mutex);
    captured_log::entries.clear();
    captured_log::throw_prefix = throwPrefix;
    captured_log::thrown = 0;
}
struct PerformanceLogFailure {
    explicit PerformanceLogFailure(const std::string &prefix) { reset_logs(prefix); }
    ~PerformanceLogFailure() {
        std::lock_guard<std::mutex> lock(captured_log::mutex);
        captured_log::throw_prefix.clear();
    }
};
std::vector<captured_log::Entry> current_logs() {
    std::lock_guard<std::mutex> lock(captured_log::mutex);
    return captured_log::entries; // 保存和性能日志现在由后台线程输出。
}
using Metrics = std::map<std::string, std::string>;
Metrics report_fields(const std::vector<captured_log::Entry> &entries, const std::string &prefix) {
    Metrics result;
    int matches = 0;
    for (const auto &entry : entries) {
        if (entry.message.rfind(prefix, 0) != 0) continue;
        ++matches;
        check(entry.level == "INFO", prefix + " 必须是 INFO 日志");
        std::string text = entry.message;
        for (char &c : text) if (c == ',' || c == ';') c = ' ';
        std::istringstream input(text);
        std::string item;
        while (input >> item) {
            const auto separator = item.find('=');
            if (separator != std::string::npos) result[item.substr(0, separator)] = item.substr(separator + 1);
        }
    }
    check(matches == 1, prefix + " 应恰好输出一条，实际 " + std::to_string(matches));
    return result;
}
std::string metric_text(const Metrics &metrics, const std::string &key) {
    const auto found = metrics.find(key);
    check(found != metrics.end() && !found->second.empty(), "性能日志缺少键 " + key);
    return found->second;
}
double metric_number(const Metrics &metrics, const std::string &key) {
    std::string text = metric_text(metrics, key);
    if (!text.empty() && text.back() == '%') text.pop_back();
    size_t consumed = 0;
    const double number = std::stod(text, &consumed);
    check(consumed == text.size() && std::isfinite(number) && number >= 0, key + " 必须是非负有限数值");
    return number;
}
void check_reports(const std::vector<captured_log::Entry> &entries, const std::string &expected, int count) {
    const auto total = report_fields(entries, "导出耗时");
    const auto parts = report_fields(entries, "保存分段");
    for (const auto *report : {&total, &parts}) {
        check(metric_text(*report, "symbol") == "600000.SH", "性能日志代码不一致");
        check(metric_text(*report, "period") == "1d", "性能日志周期不一致");
        check(metric_text(*report, "result") == expected, "性能日志结果不一致");
    }
    check(metric_text(total, "id") == metric_text(parts, "id"), "两条性能日志必须对应同一次保存");
    check(metric_number(total, "input") == count, "性能日志输入条数不一致");
    const double receive = metric_number(total, "receive_ms");
    const double submit = metric_number(total, "submit_ms");
    const double save = metric_number(total, "save_ms");
    const double combined = metric_number(total, "total_ms");
    const double wall = metric_number(total, "wall_ms");
    const double percentage = metric_number(total, "save_pct");
    check(percentage <= 100, "保存耗时占比应在 0 到 100 之间");
    // 只比较相同测量值的算术关系；容许显示舍入，不限制实际运行耗时。
    check(std::abs(receive + submit + save - combined) <= 0.25, "total_ms 应等于 receive_ms + submit_ms + save_ms");
    check(wall + 0.25 >= combined, "wall_ms 应包含接收与保存以及宿主间隔");
    check(std::abs(percentage * combined / 100 - save) <= 0.25, "save_pct 的分母应是 total_ms");
    double received = 0;
    for (const char *key : {"start_ms", "meta_ms", "date_ms", "open_ms", "close_ms", "high_ms", "low_ms", "volume_ms", "amount_ms"})
        received += metric_number(total, key);
    check(std::abs(received - receive) <= 0.25, "receive_ms 应包含开始、设置、日期及六组数组");
    double segmented = 0;
    for (const char *name : {"prepare", "read", "merge", "write"}) {
        const double time = metric_number(parts, std::string(name) + "_ms");
        const double percent = metric_number(parts, std::string(name) + "_pct");
        check(percent <= 100, "保存分段占比应在 0 到 100 之间");
        check(std::abs(percent * save / 100 - time) <= 0.25, "保存分段占比必须以 save_ms 为分母");
        segmented += time;
    }
    check(segmented <= save + 0.25, "保存分段不得超过后台保存耗时");
}
void run(const std::string &name, const std::function<void()> &fn) {
    try { fn(); ++passed; std::cout << "通过 " << name << '\n'; }
    catch (const std::exception &e) { ++failed; std::cout << "失败 " << name << "：" << e.what() << '\n'; }
}
std::string header(const std::string &key = "") { return "symbol,period,ts,open,high,low,close,volume,amount" + (key.empty() ? "" : "," + key) + "\n"; }
std::string row(const std::string &period = "1d", const std::string &ts = "2026-10-16 15:00:00", const std::string &key = "") {
    return "600000.SH," + period + "," + ts + ",10,12,9,11,100,1000" + (key.empty() ? "" : "," + key) + "\n";
}
bool high_address_usage(std::uint64_t &total,std::uint64_t &available,std::uint64_t &commit) {
    total=100000;available=5000;commit=100000;return true; // 地址已占95%，系统提交额度仍充足。
}
bool constrained_memory(std::uint64_t &total,std::uint64_t &available,std::uint64_t &commit) {
    total=100000;available=100000;commit=0;return true;
}
bool allocation_memory(std::uint64_t &total,std::uint64_t &available,std::uint64_t &commit) {
    total=1ULL<<40;available=total;commit=total;
    if(++allocation_fault::probe_calls%allocation_fault::stride.load()==0&&allocation_fault::probe_failures.load()!=0) {
        if(allocation_fault::probe_failures.load()>0)--allocation_fault::probe_failures;
        allocation_fault::fail_next=true;
    }
    return true;
}
struct AllocationControl {
    AllocationControl() {allocation_fault::triggered=0;allocation_fault::probe_calls=0;}
    ~AllocationControl() {allocation_fault::fail_next=false;allocation_fault::fail_after=-1;allocation_fault::probe_failures=0;allocation_fault::temp_failures=0;}
};
struct InjectedWriter {
    explicit InjectedWriter(tdx_export::AsyncWriter::MemoryProbe probe) {
        call(Func7,1,{0},{0},{0});delete export_writer;export_writer=nullptr;
        export_writer=new tdx_export::AsyncWriter(perform_save,probe);
    }
    ~InjectedWriter() {
        call(Func7,1,{0},{0},{0});delete export_writer;export_writer=nullptr;
    }
};
void wait_for_condition(const std::function<bool()> &ready,const std::string &message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    std::mutex mutex;std::condition_variable wake;std::unique_lock<std::mutex> lock(mutex);
    while(!ready()) {check(std::chrono::steady_clock::now()<deadline,message);wake.wait_for(lock,std::chrono::milliseconds(20));}
}

size_t status_files(const fs::path &directory) {
    size_t count=0;
    for(const auto &file:fs::directory_iterator(directory))
        if(file.is_regular_file()&&(file.path().filename().string().rfind("_bg_",0)==0||file.path().filename().string().rfind("bg_",0)==0))++count;
    return count;
}
BY_HANDLE_FILE_INFORMATION file_identity(const fs::path &path) {
    FileHandle handle(CreateFileA(path.string().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};check(GetFileInformationByHandle(handle.value,&info)!=FALSE,"无法取得状态文件标识");return info;
}
struct SavingBarrier {
    SavingBarrier(){std::lock_guard<std::mutex> lock(captured_log::mutex);captured_log::held=0;captured_log::save_permits=0;captured_log::hold_saved=true;}
    void one(){std::lock_guard<std::mutex> lock(captured_log::mutex);++captured_log::save_permits;captured_log::changed.notify_all();}
    void all(){std::lock_guard<std::mutex> lock(captured_log::mutex);captured_log::hold_saved=false;captured_log::save_permits=0;captured_log::changed.notify_all();}
    ~SavingBarrier(){all();}
};

void suite() {
    run("后台启动兼容旧前缀且停机后保留唯一空状态文件",[]{
        Fixture f("[async]\nqueue_capacity=3\nworker_threads=2\n");const auto dir=f.dir/"csv";
        fs::create_directory(dir/"_bg_目录不得改名");write(dir/"bg_previous","");const auto old=file_identity(dir/"bg_previous");
        Batch b;const auto marker=dir/"_bg_0-3_0-2";wait_for_condition([&]{return fs::exists(marker);},"后台启动没有发布空闲状态");
        const auto current=file_identity(marker);check(old.dwVolumeSerialNumber==current.dwVolumeSerialNumber&&old.nFileIndexHigh==current.nFileIndexHigh&&old.nFileIndexLow==current.nFileIndexLow,"必须改名复用原文件而非重建");
        b.complete();check(fs::file_size(marker)==0&&status_files(dir)==1,"状态文件必须为空且只有一个");check(fs::is_directory(dir/"_bg_目录不得改名"),"不得重命名同前缀目录");
    });
    run("后台状态文件可在尚未存在的多级csv目录创建",[]{
        Fixture f;write(f.dir/"saveAsCVS.ini","[log]\nmode=disable\n[export]\ncsv_dir=deep/new/csv\n");
        Batch b;const auto dir=f.dir/"deep"/"new"/"csv";wait_for_condition([&]{return fs::exists(dir/"_bg_0-1000_0-8");},"未在新输出目录发布状态");
        b.complete();check(status_files(dir)==1&&fs::file_size(dir/"_bg_0-1000_0-8")==0,"排空后必须保留零状态");
        contains(read(dir/"600000.SH.1d.csv"),"2026-10-16");
    });
    run("复用已有非空bg文件只改名且不截断原内容",[]{
        Fixture f;const std::string content="既有文件内容不得被截断";write(f.dir/"csv"/"_bg_previous.txt",content);
        Batch b;const auto marker=f.dir/"csv"/"_bg_0-1000_0-8";wait_for_condition([&]{return fs::exists(marker);},"未复用既有文件");
        b.complete();check(read(marker)==content&&status_files(f.dir/"csv")==1,"已有文件只应更新文件名而不修改内容");
    });
    run("状态文件忙碌时按五秒刷新排空即归零空闲暂停并可恢复",[]{
        Fixture f("[async]\nqueue_capacity=3\nworker_threads=2\n");SavingBarrier barrier;const auto dir=f.dir/"csv";
        Batch first;wait_for_condition([&]{return fs::exists(dir/"_bg_0-3_0-2");},"启动状态缺失");
        auto submit=[&](Batch &b){status(b.dates({1261016}),1,"状态样例日期");b.data();status(call(Func1,1,{3},b.previous,{0}),1,"状态样例提交");};
        submit(first);Batch second(1,5,0,600001);submit(second);Batch third(1,5,0,600002);submit(third);
        wait_for_condition([&]{return fs::exists(dir/"_bg_1-3_2-2");},"未区分一个等待任务与两个运行任务");
        barrier.one();wait_for_condition([&]{return fs::exists(dir/"_bg_0-3_2-2");},"忙碌期间未刷新等待队列数量");
        barrier.one();wait_for_condition([&]{return fs::exists(dir/"_bg_0-3_1-2");},"队列已空但还有运行任务时未继续刷新");
        barrier.all();const auto idle=dir/"_bg_0-3_0-2";wait_for_condition([&]{return fs::exists(idle)&&writer().status().unfinished==0;},"全部完成没有固定归零");
        fs::rename(idle,dir/"_bg_idle_probe");std::mutex mutex;std::condition_variable wake;std::unique_lock<std::mutex> lock(mutex);wake.wait_for(lock,std::chrono::seconds(6));
        check(fs::exists(dir/"_bg_idle_probe")&&!fs::exists(idle),"空闲后不得仍按五秒刷新");
        {std::lock_guard<std::mutex> held(captured_log::mutex);captured_log::hold_saved=true;}
        Batch resumed(1,5,0,600003);submit(resumed);wait_for_condition([&]{return fs::exists(dir/"_bg_0-3_1-2");},"新任务未恢复状态更新");
        barrier.all();status(call(Func7,1,{0},{0},{0}),1,"状态线程随所有工作线程排空退出");
        check(fs::exists(idle)&&status_files(dir)==1,"停机应保留唯一零状态");
    });
    run("状态文件改名失败不影响CSV保存及错误汇总",[]{
        Fixture f;const auto marker=f.dir/"csv"/"_bg_locked";write(marker,"");reset_logs();
        {FileHandle hold(CreateFileA(marker.string().c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr));Batch b;b.complete();}
        check(fs::exists(marker)&&fs::file_size(marker)==0,"状态改名失败不得破坏原文件");contains(read(f.file()),"2026-10-16");
        status(call(Func6,1,{3},{0},{0}),0,"状态展示错误不能伪报行情保存失败");
        bool reported=false;for(const auto &entry:current_logs())if(entry.message.rfind("后台状态文件更新失败",0)==0)reported=true;
        check(reported,"状态更新失败必须有日志");
    });
    run("运行中切换csv目录后两个目录最终状态均归零",[]{
        Fixture f;SavingBarrier barrier;Batch first;
        auto submit=[&](Batch &b){status(b.dates({1261016}),1,"目录样例日期");b.data();status(call(Func1,1,{3},b.previous,{0}),1,"目录样例提交");};
        wait_for_condition([&]{return fs::exists(f.dir/"csv"/"_bg_0-1000_0-8");},"原目录启动状态缺失");submit(first);
        write(f.dir/"saveAsCVS.ini","[log]\nmode=disable\n[export]\ncsv_dir=another/csv\n");Batch second(1,5,0,600001);
        const auto other=f.dir/"another"/"csv";wait_for_condition([&]{return fs::exists(other)&&status_files(other)==1;},"新目录未注册状态文件");submit(second);
        barrier.all();status(call(Func7,1,{0},{0},{0}),1,"切换目录后排空");
        for(const auto &dir:{f.dir/"csv",other})check(fs::exists(dir/"_bg_0-1000_0-8")&&status_files(dir)==1,"每个使用过的目录均须最终归零");
        contains(read(f.file()),"600000.SH");contains(read(other/"600001.SH.1d.csv"),"600001.SH");
    });
    for(int field=1;field<=6;++field)for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()})
    run("后台逐列拒绝非法数值 "+std::to_string(field)+" "+std::to_string(invalid),[=]{
        Fixture f;const auto original=header()+row("1d","2026-10-15 15:00:00");write(f.file(),original);
        Batch b;status(b.dates({1261016}),1,"原始日期已接收");
        for(int type=1;type<=6;++type){float v[]={0,10,11,12,9,100,1000};status(b.field(type,{type==field?invalid:v[type]}),1,"原始数值已接收");}
        status(b.save(),-1,"后台不得写入非法数值");check(read(f.file())==original,"非法数值不得改变原文件");
        status(call(Func6,1,{3},{0},{0}),1,"非法数值须保留失败");
    });
    run("无效任务不覆盖历史且后续其他股票继续保存",[]{
        Fixture f;const auto original=header()+row("1d","2026-10-15 15:00:00");write(f.file(),original);
        Batch bad;status(bad.dates({1261016}),1,"日期");
        for(int type=1;type<=6;++type){float v[]={0,10,11,12,9,100,1000};status(bad.field(type,{type==5?-1.f:v[type]}),1,"原始量额");}
        status(call(Func1,1,{3},bad.previous,{0}),1,"非法内容仍安全移交后台");
        Batch good(1,5,0,600001);status(good.dates({1261019}),1,"后续日期");good.data();status(call(Func1,1,{3},good.previous,{0}),1,"后续正常提交");
        status(call(Func7,1,{0},{0},{0}),-1,"整体排空应报告前一个任务失败");
        check(read(f.file())==original,"错误任务不得改变旧文件");contains(read(f.file("1d","600001.SH")),"2026-10-19");
        const auto stats=writer().status();check(stats.accepted==2&&stats.failed==1&&stats.succeeded==1&&!stats.bytes&&!stats.unfinished,"已接受、成功、失败与额度必须一致");
    });
    for(bool time_field:{false,true})for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::max(),1261016.5f})
    run(std::string("后台安全拒绝原始浮点")+(time_field?"时间 ":"日期 ")+std::to_string(invalid),[=]{
        Fixture f;const auto original=header()+row("15m","2026-10-15 09:30:00");write(f.file("15m"),original);
        Batch b(1,2);status(b.dates({time_field?1261016.f:invalid},{time_field?invalid:930.f}),1,"原始数组接收成功");b.data();
        status(b.save(),-1,"后台校验必须在不安全整数转换之前失败");
        check(read(f.file("15m"))==original,"非法浮点不得覆盖原文件");
        status(call(Func6,1,{3},{0},{0}),1,"后台错误仍可查询");status(call(Func6,1,{3},{0},{0}),1,"查询不清除后台错误");
    });
    for(int kind:{0,1,2})run("后台拒绝日期时间乱序或重复 "+std::to_string(kind),[=]{
        Fixture f;const auto original=header()+row("15m","2026-10-14 09:30:00");write(f.file("15m"),original);
        Batch b(2,2);const Series dates=kind==0?Series{1261016,1261015}:Series{1261016,1261016};
        const Series times=kind==2?Series{945,945}:Series{945,930};
        status(b.dates(dates,times),1,"未解析数组已接收");b.data();status(b.save(),-1,"后台必须拒绝乱序重复");
        check(read(f.file("15m"))==original,"后台顺序检查失败不能覆盖历史");
    });
    run("前台复制原始数组后调用方改写不会影响后台",[]{
        Fixture f;Batch b(2,2);Series dates{1261016,1261019},times{930,945},out(2,-999);
        // 直接传入随后改写的同一缓冲，避免按值调用助手的副本掩盖所有权错误。
        Func2(2,out.data(),dates.data(),times.data(),b.previous.data());b.previous=out;status(out,1,"复制原始日期时间");
        std::fill(dates.begin(),dates.end(),std::numeric_limits<float>::quiet_NaN());std::fill(times.begin(),times.end(),-1.f);
        for(int type=1;type<=6;++type){
            float value[]={0,10,11,12,9,100,1000};Series data(2,value[type]),types(2,static_cast<float>(type));
            Func3(2,out.data(),types.data(),data.data(),b.previous.data());b.previous=out;status(out,1,"复制数值");
            std::fill(data.begin(),data.end(),std::numeric_limits<float>::quiet_NaN());
        }
        status(b.save(),1,"调用方改写不能污染任务");const auto csv=read(f.file("15m"));check(lines(csv)==3,"两根数据均须保存");
        contains(csv,"2026-10-16 09:30:00");contains(csv,"2026-10-19 09:45:00");
    });
    for(int missing:{0,1,2})run("后台独立检查任务数组完整性 "+std::to_string(missing),[=]{
        Fixture f;Batch b(1,2);status(b.dates({1261016},{930}),1,"日期");b.data();auto item=static_cast<const ExportJob &>(state());
        if(missing==0)item.dates.clear();else if(missing==1)item.times.clear();else item.series[5].clear();
        check(!write_csv(item),"缺少任务数组必须在读取之前拒绝");check(!fs::exists(f.file("15m")),"不完整任务不能落盘");
    });
    for(int invalid:{0,-1,1230229,1260230,1261331})run("后台入口独立拒绝非法原始日期 "+std::to_string(invalid),[=]{
        Fixture f;const auto original=header()+row("1m","2026-10-15 09:30:00");write(f.file("1m"),original);
        Batch b(1,0);status(b.dates({1261016},{930}),1,"准备正常任务");b.data();
        auto item=static_cast<const ExportJob &>(state());item.dates[0]=static_cast<float>(invalid);
        check(!write_csv(item),"后台必须独立拒绝非法日期");check(read(f.file("1m"))==original,"后台日期错误不得改变原文件");
    });
    run("后台入口独立拒绝非法原始分钟时间",[]{
        Fixture f;const auto original=header()+row("1m","2026-10-15 09:30:00");write(f.file("1m"),original);
        Batch b(1,0);status(b.dates({1261016},{930}),1,"准备正常任务");b.data();
        auto item=static_cast<const ExportJob &>(state());item.times[0]=1260;
        check(!write_csv(item),"后台必须独立拒绝非法HHMM");check(read(f.file("1m"))==original,"后台时间错误不得改变原文件");
    });
    for(const std::string count:{"0","129","invalid"})run("非法保存线程配置拒绝导出 "+count,[=]{
        Fixture f("[async]\nworker_threads="+count+"\n");
        status(call(Func4,1,{600000},{1},{5}),-1,"非法线程配置");
        status(call(Func6,1,{3},{0},{0}),1,"配置失败必须保留");
        status(call(Func7,1,{0},{0},{0}),-1,"配置失败不能报告成功");
        check(!fs::exists(f.file()),"非法线程配置不能生成CSV");
    });
    for(int workers:{8,32,128})run(std::to_string(workers)+"线程实际保存时状态查询区分正在保存与等待任务",[=]{
        Fixture f(workers==8?"":"[async]\nworker_threads="+std::to_string(workers)+"\n");
        struct HeldLogs {
            HeldLogs(){std::lock_guard<std::mutex> lock(captured_log::mutex);captured_log::held=0;captured_log::hold_saved=true;}
            void release(){std::lock_guard<std::mutex> lock(captured_log::mutex);captured_log::hold_saved=false;captured_log::changed.notify_all();}
            ~HeldLogs(){release();}
        } gate;
        for(int i=0;i<workers;++i){Batch b(1,5,0,600000+i);status(b.dates({1261016}),1,"并行日期");b.data();status(call(Func1,1,{3},b.previous,{0}),1,"并行提交");}
        {
            std::unique_lock<std::mutex> lock(captured_log::mutex);
            check(captured_log::changed.wait_for(lock,std::chrono::seconds(10),[=]{return captured_log::held==workers;}),"配置的真实保存任务未同时到达结束屏障");
        }
        status(call(Func6,1,{0},{0},{0}),static_cast<float>(workers),"未完成包含所有正在保存");
        status(call(Func6,1,{2},{0},{0}),static_cast<float>(workers),"正在保存必须等于配置线程数");
        status(call(Func6,1,{1},{0},{0}),0,"全部在保存时队列等待为0");
        Batch extra(1,5,0,600000+workers);status(extra.dates({1261016}),1,"额外任务日期");extra.data();status(call(Func1,1,{3},extra.previous,{0}),1,"额外任务提交");
        status(call(Func6,1,{0},{0},{0}),static_cast<float>(workers+1),"含等待任务的未完成数");
        status(call(Func6,1,{1},{0},{0}),1,"额外任务必须等待");
        status(call(Func6,1,{2},{0},{0}),static_cast<float>(workers),"不得超过配置的保存并发上限");
        gate.release();status(call(Func7,1,{0},{0},{0}),1,"全部工作线程排空");
        for(int i=0;i<=workers;++i)check(lines(read(f.file("1d",std::to_string(600000+i)+".SH")))==2,"每个目标必须完整保存");
        check(fs::exists(f.dir/"csv"/("_bg_0-1000_0-"+std::to_string(workers))),"状态文件须显示实际线程上限及最终归零");
    });
    struct Period { int code; int reserved; const char *label; };
    for (auto p : std::vector<Period>{{0,0,"1m"},{1,0,"5m"},{2,0,"15m"},{3,0,"30m"},{4,0,"60m"},{5,0,"1d"},{6,0,"1w"},{7,0,"1mo"},{11,0,"1y"}}) {
        run(std::string("支持周期 ") + p.label, [=] {
            Fixture f; Batch b(2, p.code, p.reserved);
            b.complete({1261015,1261016}, {930,1030});
            std::string csv = read(f.file(p.label));
            check(csv.find('+') == std::string::npos, "新时间戳不得带时区");
            bool minute = p.code <= 4 || p.reserved != 0;
            contains(csv, minute ? "2026-10-16 10:30:00" : "2026-10-16 15:00:00");
            check(lines(csv) == (p.code == 6 || p.code == 7 || p.code == 11 ? 2u : 3u), "输出行数不正确");
        });
    }

    run("15分钟同日合并更新重叠时间并保留历史",[]{
        Fixture f;Batch first(2,2);first.complete({1261016,1261016},{930,945});
        Batch next(2,2);status(next.dates({1261016,1261016},{945,1000}),1,"15分钟接收时间");
        for(int type=1;type<=6;++type){float value[]={0,10,11,12,9,100,1000};status(next.field(type,type==5?Series{200,300}:values(2,value[type])),1,"15分钟字段");}
        status(next.save(),1,"15分钟合并");const auto csv=read(f.file("15m"));
        check(lines(csv)==4,"重叠15分钟应更新且保留三个时间点");
        contains(csv,"600000.SH,15m,2026-10-16 09:30:00,10,12,9,11,100,1000");
        contains(csv,"600000.SH,15m,2026-10-16 09:45:00,10,12,9,11,200,1000");
        contains(csv,"600000.SH,15m,2026-10-16 10:00:00,10,12,9,11,300,1000");
        check(!fs::exists(f.file("5m")),"15分钟不能混入5分钟文件");
    });
    for(bool duplicate:{false,true})run(duplicate?"15分钟重复时间拒绝且保护已有文件":"15分钟非法HHMM拒绝且保护已有文件",[=]{
        Fixture f;Batch original(1,2);original.complete({1261016},{930});const auto before=read(f.file("15m"));
        Batch bad(2,2);status(bad.dates({1261016,1261016},duplicate?Series{945,945}:Series{945,1260}),1,"15分钟原始时间已接收");bad.data();
        status(bad.save(),-1,"15分钟后台必须拒绝非法时间");check(read(f.file("15m"))==before,"错误输入不能改变15分钟旧文件");
    });
    run("长周期忽略无效 TIME", [] { Fixture f; Batch b; b.complete({1261016},{std::numeric_limits<float>::quiet_NaN()}); contains(read(f.file()), "15:00:00"); });
    run("长周期无 TIME 仍可保存", [] { Fixture f; Batch b; b.complete(); contains(read(f.file()), "15:00:00"); });
    run("按批读取默认时间并忽略旧时区配置", [] { Fixture f("tz=-05\n"); write(f.dir / "saveAsCVS.ini", "[export]\ncsv_dir=csv\ndefault_time=145959\ntz=-05\n"); Batch b; b.complete(); auto csv=read(f.file()); contains(csv,"14:59:59"); check(csv.find("-05") == std::string::npos,"时区配置不得影响新输出"); });
    run("配置不可读不得静默使用默认目录和时间", [] { Fixture f; write(f.dir/"saveAsCVS.ini","[export]\ncsv_dir=required-directory\ndefault_time=145959\n"); FileHandle lock(CreateFileA((f.dir/"saveAsCVS.ini").string().c_str(),GENERIC_WRITE,FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr)); status(call(Func4,1,{600000},{1},{5}),-1,"配置不可读"); check(!fs::exists(f.file()),"配置错误不得静默写入默认目录"); });
    run("保持六位有效数字输出", [] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期"); for(int t=1;t<=6;++t) { float v[]={0,10,11,12,9,12345678,1000}; status(b.field(t,{v[t]}),1,"字段"); } status(b.save(),1,"保存"); contains(read(f.file()),"1.23457e+07"); });
    run("有限极值保持六位设计并可再次合并", [] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期"); for(int type=1;type<=6;++type) { float v[]={0,10,11,12,9,100,std::numeric_limits<float>::max()}; status(b.field(type,{v[type]}),1,"有限数值"); } status(b.save(),1,"有限极值保存"); contains(read(f.file()),"3.40282e+38"); Batch next; next.complete({1261019}); check(lines(read(f.file()))==3,"格式化后的极值必须能被完整读取并合并"); });
    run("跨年 ISO 周年不碰撞", [] { Fixture f; Batch b(2,6); b.complete({1190104,1191231}); auto csv=read(f.file("1w")); check(lines(csv)==3,"两个 ISO 周都须保留"); contains(csv,"2019W01"); contains(csv,"2020W01"); });
    run("同一跨年 ISO 周只保留较晚日期", [] { Fixture f; Batch b(2,6); b.complete({1201231,1210101}); auto csv=read(f.file("1w")); check(lines(csv)==2,"同一 ISO 周应合并"); contains(csv,"2020W53"); contains(csv,"2021-01-01"); });
    for (const char *suffix : {"+08", "+08:00"}) run(std::string("兼容历史时区 ") + suffix, [=] {
        Fixture f; write(f.file(),header()+row("1d",std::string("2026-10-15 15:00:00")+suffix)); Batch b(2); b.complete({1261015,1261016}); auto csv=read(f.file()); check(lines(csv)==3,"旧格式和新格式应按相同时间合并"); check(csv.find('+')==std::string::npos,"重写时应移除旧时区");
    });
    run("增量保存保留历史日期", [] { Fixture f; write(f.file(),header()+row("1d","2026-10-15 15:00:00")); Batch b; b.complete(); auto csv=read(f.file()); check(lines(csv)==3,"新旧两日都应保留"); contains(csv,"2026-10-15"); contains(csv,"2026-10-16"); });
    run("相同时间可更新行情而不重复行", [] { Fixture f; write(f.file(),header()+"600000.SH,1d,2026-10-16 15:00:00,10,12,9,10,100,1000\n"); Batch b; b.complete(); auto csv=read(f.file()); check(lines(csv)==2,"相同时间只能一行"); contains(csv,",10,12,9,11,"); });
    run("旧错误周年按日期重新计算", [] { Fixture f; write(f.file("1w"),header("week_key")+row("1w","2019-01-04 15:00:00+08","2019W01")+row("1w","2019-12-31 15:00:00+08","2019W01")); Batch b(1,6); b.complete({1200103}); auto csv=read(f.file("1w")); check(lines(csv)==3,"不能继续信任旧错误周年而吞行"); contains(csv,"2019W01"); contains(csv,"2020W01"); contains(csv,"2020-01-03"); });
    run("旧公式控制顺序必须明确失败", [] { Fixture f; status(call(Func1,1,{0},{0},{0}),-1,"旧重置"); status(call(Func1,1,{1},{600000},{1}),-1,"旧代码设置"); status(call(Func1,1,{2},{5},{0}),-1,"旧周期设置"); check(!fs::exists(f.file()),"旧调用不得生成文件"); });
    run("空批次被拒绝", [] { Fixture f; auto result=call(Func4,0,{600000},{1},{5}); check(result[0]<=0,"零长度不得返回有效批次"); status(call(Func1,1,{3},{result[0]},{0}),-1,"空批次保存"); check(!fs::exists(f.file()),"空批次不得落盘"); });
    for(int period : {8,9,10,12,13,14}) run("拒绝未支持周期 " + std::to_string(period), [=] {
        Fixture f; auto out=call(Func4,1,{600000},{1},{static_cast<float>(period)}); if(out[0]==1) status(call(Func5,1,out,{0},{0}),-1,"未支持周期"); else status(out,-1,"未支持周期"); check(!fs::exists(f.file("unknown")),"不得写 unknown 文件");
    });
    run("未知市场被拒绝", [] { Fixture f; auto out=call(Func4,1,{600000},{99},{5}); if(out[0]==1) status(call(Func5,1,out,{0},{0}),-1,"市场"); else status(out,-1,"市场"); });
    for(int flag : {1,2,3}) run("拒绝复权标志 " + std::to_string(flag), [=] { Fixture f; auto out=call(Func4,1,{600000},{1},{5}); status(out,1,"开始导出"); auto setting=call(Func5,1,out,{static_cast<float>(flag)},{0}); status(setting,-1,"复权"); status(call(Func1,1,{3},setting,{0}),-1,"失败导出保存"); check(!fs::exists(f.file()),"复权不得落盘"); });
    run("普通周期保留参数必须为零", [] { Fixture f; auto out=call(Func4,1,{600000},{1},{4}); status(out,1,"开始导出"); status(call(Func5,1,out,{0},{120}),-1,"非零保留参数"); });
    run("多分钟周期不再支持", [] { Fixture f; auto out=call(Func4,1,{600000},{1},{8}); if(out[0]==1) status(call(Func5,1,out,{0},{0}),-1,"未支持周期"); else status(out,-1,"未支持周期"); });
    for(float date : {0.f,1260230.f,1230229.f,1261331.f,-1.f}) run("非法日期 " + std::to_string(static_cast<int>(date)), [=] { Fixture f; Batch b; status(b.dates({date}),1,"原始日期已接收");b.data();status(b.save(),-1,"后台拒绝非法日期"); check(!fs::exists(f.file()),"坏日期不得落盘"); });
    run("闰日可保存", [] { Fixture f; Batch b; b.complete({1240229}); contains(read(f.file()),"2024-02-29"); });
    for(float time : {2460.f,1260.f,-1.f,930.5f}) run("非法分钟时间 " + std::to_string(time), [=] { Fixture f; Batch b(1,0); status(b.dates({1261016},{time}),1,"原始分钟时间已接收");b.data();status(b.save(),-1,"后台拒绝非法时间"); });
    run("分钟线缺少 TIME 不得保存", [] { Fixture f; Batch b(1,0); status(b.dates({1261016}),-1,"缺少分钟时间"); status(b.save(),-1,"失败批保存"); });
    for(float invalid : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) run(std::isnan(invalid)?"拒绝 NaN":"拒绝 Inf", [=] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期");for(int t=1;t<=6;++t){float v[]={0,10,11,12,9,100,1000};status(b.field(t,{t==1?invalid:v[t]}),1,"原始数值已接收");}status(b.save(),-1,"后台拒绝非法数值"); });
    for(int type : {5,6}) run("拒绝负量额字段 " + std::to_string(type), [=] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期");for(int t=1;t<=6;++t){float v[]={0,10,11,12,9,100,1000};status(b.field(t,{t==type?-1.f:v[t]}),1,"原始量额已接收");}status(b.save(),-1,"后台拒绝负量额"); });
    run("价格上下界矛盾禁止保存", [] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期"); bool rejected=false; for(int t=1;t<=6;++t) { float v[]={0,10,11,9,8,100,1000}; auto result=b.field(t,{v[t]}); if(result[0]==-1) { rejected=true; break; } status(result,1,"字段"); } status(b.save(),-1,"矛盾价格保存"); check(!fs::exists(f.file()),"矛盾价格不得落盘"); });
    run("缺字段不能保存", [] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期"); status(b.field(1,{10}),1,"开盘"); status(b.save(),-1,"缺字段"); });
    run("字段乱序使批次永久失败", [] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期"); status(b.field(2,{11}),-1,"乱序"); status(b.field(1,{10}),-1,"失败批继续传"); status(b.save(),-1,"失败批保存"); });
    run("长度变化不能清空数据后成功保存", [] { Fixture f; Batch b(2); status(b.dates({1261015,1261016}),1,"日期"); status(b.field(1,{10,10}),1,"开盘"); status(b.field(2,{11},1),-1,"改变长度"); status(b.save(),-1,"失败批保存"); });
    run("一个批次只能保存一次", [] { Fixture f; Batch b; b.complete(); auto old=read(f.file()); status(b.save(),-1,"重复保存"); check(read(f.file())==old,"重复保存不得改文件"); });
    run("重新开始清空旧股票及旧数组", [] { Fixture f; Batch old(2); old.complete({1261015,1261016}); const auto original=read(f.file()); Batch current(1,5,0,600001); status(current.dates({1261019}),1,"新股票日期"); status(current.field(1,{20}),1,"新股票开盘"); status(current.save(),-1,"新股票缺字段"); status(call(Func7,1,{0},{0},{0}),-1,"先报告缺字段失败"); check(!fs::exists(f.file("1d","600001.SH")),"新股票不得沿用旧数组补全"); check(read(f.file())==original,"新股票处理不能改写旧股票"); Batch retry(1,5,0,600001); retry.complete({1261019}); auto csv=read(f.file("1d","600001.SH")); check(lines(csv)==2,"新股票只能包含本次数据"); contains(csv,"600001.SH,1d,2026-10-19"); check(csv.find("600000.SH")==std::string::npos,"旧代码不得残留"); });
    run("失败后重新开始可完整恢复", [] { Fixture f; Batch bad; status(bad.dates({1261016}),1,"日期"); status(bad.field(2,{11}),-1,"故意乱序"); status(bad.field(1,{10}),-1,"失败后不可续传"); status(call(Func7,1,{0},{0},{0}),-1,"失败轮次必须先明确报告"); Batch recovered; recovered.complete({1261019}); check(lines(read(f.file()))==2,"新开始应清除失败状态"); });
    for(int stage : {5,2,3,1}) run("上一步失败阻断函数 " + std::to_string(stage), [=] { Fixture f; if(stage==5) { auto start=call(Func4,1,{600000},{1},{5}); status(start,1,"开始导出"); status(call(Func5,1,{-1},{0},{0}),-1,"失败依赖"); status(call(Func5,1,start,{0},{0}),-1,"本次不可恢复续传"); } else { Batch b; if(stage!=2) status(b.dates({1261016}),1,"日期"); if(stage==1) b.data(); if(stage==2) status(call(Func2,1,{1261016},{0},{-1}),-1,"失败日期依赖"); if(stage==3) status(call(Func3,1,{1},{10},{-1}),-1,"失败字段依赖"); if(stage==1) status(call(Func1,1,{3},{-1},{0}),-1,"失败保存依赖"); status(b.save(),-1,"本次不可恢复提交"); } check(!fs::exists(f.file()),"上一步失败不得落盘"); });
    for(float dependency : {0.f,2.f,1000000.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) run("前一步结果必须精确为一 " + std::to_string(dependency), [=] { Fixture f; Batch b; status(b.dates({1261016}),1,"日期"); status(call(Func3,1,{1},{10},{dependency}),-1,"非一依赖"); status(b.field(1,{10}),-1,"错误依赖后不可续传"); status(b.save(),-1,"错误依赖后不可保存"); check(!fs::exists(f.file()),"非一依赖不得落盘"); });
    run("五次重叠导出只有一个文件并保留历史", [] { Fixture f; const Series dates[]={ {1261015,1261016},{1261016,1261019},{1261019,1261020},{1261020,1261021},{1261021,1261022} }; for(const auto &date:dates) { Batch b(2); b.complete(date); } size_t files=0; for(const auto &entry:fs::directory_iterator(f.dir/"csv")) if(entry.path().extension()==".csv") ++files; check(files==1,"重复开始不能生成带编号的新文件"); const auto csv=read(f.file()); check(lines(csv)==7,"重叠历史应去重并保留六个日期"); contains(csv,"2026-10-15"); contains(csv,"2026-10-22"); });
    run("旧 CSV 超出浮点范围必须拒绝", [] { Fixture f; auto original=header()+"600000.SH,1d,2026-10-15 15:00:00,10,12,9,11,100,1e100\n"; write(f.file(),original); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"超范围数值"); check(read(f.file())==original,"不能将超范围数值截成最大值再覆盖"); });
    run("重新开始允许更改数组长度", [] { Fixture f; Batch old(2); status(old.dates({1261015,1261016}),1,"旧长度日期"); status(old.field(1,{10,10}),1,"旧长度开盘"); Batch current(1); current.complete({1261019}); const auto csv=read(f.file()); check(lines(csv)==2,"重新开始必须丢弃旧长度数据"); check(csv.find("2026-10-15")==std::string::npos,"旧数组不得残留"); });
    run("当前导出的零长度调用使本次失败", [] { Fixture f; Batch b; float out=-999, type=1, data=10, previous=b.previous[0]; Func3(0,&out,&type,&data,&previous); status(b.dates({1261016}),-1,"零长度后继续输入"); status(b.save(),-1,"零长度失败后保存"); check(!fs::exists(f.file()),"无效导出不得落盘"); });
    run("旧 CSV 最后一行没有换行仍完整读取", [] { Fixture f; auto original=header()+row("1d","2026-10-15 15:00:00"); original.pop_back(); write(f.file(),original); Batch b; b.complete(); check(lines(read(f.file()))==3,"不得丢弃末尾没有换行的历史行"); });
    run("其他时区不得静默迁移", [] { Fixture f; auto original=header()+row("1d","2026-10-15 15:00:00-05"); write(f.file(),original); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"未支持时区"); check(read(f.file())==original,"其他时区原文件必须保留"); });
    for(int period : {8,14}) run("取消旧120分钟声明 "+std::to_string(period), [=] { Fixture f; auto out=call(Func4,1,{600000},{1},{static_cast<float>(period)}); if(out[0]==1) status(call(Func5,1,out,{0},{120}),-1,"已取消周期"); else status(out,-1,"已取消周期"); check(!fs::exists(f.file("120m")),"不得继续生成120分钟文件"); });
    run("旧非零极小数不得静默变成零", [] { Fixture f; auto original=header()+"600000.SH,1d,2026-10-15 15:00:00,10,12,9,11,100,1e-100\n"; write(f.file(),original); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"下溢数值"); check(read(f.file())==original,"下溢时必须保留原文件"); });
    run("数值解析不申请临时堆内存且兼容符号和空白",[]{
        const std::string text=" \t+1.25e+03\r\n";float value=0;const auto before=allocation_fault::allocations;
        const auto ok=parse_number(text,value);const auto used=allocation_fault::allocations-before;
        check(ok&&value==1250,"兼容原数字语义");check(used==0,"解析每个数字不应再建立流或申请堆内存");
    });
    run("固定时间和普通周期合并键只分配结果字符串",[]{
        const auto before=allocation_fault::allocations;const auto ts=format_ts(20261020,93000);const auto used=allocation_fault::allocations-before;
        check(ts=="2026-10-20 09:30:00"&&used<=1,"固定时间不应创建文本流或申请额外堆内存");
        for(const std::string period:{"1m","5m","15m","30m","60m","1d"}){
            const auto start=allocation_fault::allocations;const auto key=row_key(period,20261020,ts);const auto count=allocation_fault::allocations-start;
            check(key==ts&&count<=1,"普通周期不能为了返回时间戳而构造无用文本流");
        }
    });
    run("旧负极小量额不得以负零通过", [] { Fixture f; auto original=header()+"600000.SH,1d,2026-10-15 15:00:00,10,12,9,11,-1e-100,1000\n"; write(f.file(),original); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"负数下溢"); check(read(f.file())==original,"负量额下溢时必须保留原文件"); });
    run("只有表头的旧文件不能视为完整历史", [] { Fixture f; auto original=header(); write(f.file(),original); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"缺少旧数据行"); check(read(f.file())==original,"不完整文件必须原样保留"); });
    run("旧同时间冲突记录不能静默选最后一条", [] { Fixture f; auto original=header()+row()+"600000.SH,1d,2026-10-16 15:00:00,10,12,9,10,100,1000\n"; write(f.file(),original); Batch b; status(b.dates({1261019}),1,"日期"); b.data(); status(b.save(),-1,"冲突旧记录"); check(read(f.file())==original,"冲突文件必须原样保留"); });
    run("旧完全相同的重复记录允许合并", [] { Fixture f; write(f.file(),header()+row()+row()); Batch b; b.complete({1261019}); check(lines(read(f.file()))==3,"相同重复行应去重且保留新日期"); });
    run("旧大周期同日期冲突记录不能静默选最后", [] { Fixture f; auto original=header("month_key")+row("1mo","2026-10-16 00:00:00+08","202610")+"600000.SH,1mo,2026-10-16 15:00:00+08,10,12,9,10,100,1000,202610\n"; write(f.file("1mo"),original); Batch b(1,7); status(b.dates({1261019}),1,"日期"); b.data(); status(b.save(),-1,"规范化后冲突"); check(read(f.file("1mo"))==original,"规范化不能掩盖旧值冲突"); });
    run("输出超过读取上限不得替换旧文件", [] { Fixture f; auto original=header()+row(); write(f.file(),original); Batch b; status(b.dates({1261019}),1,"日期"); b.data(); Series result; simulate_oversized_file=true; try { result=b.save(); } catch(...) { simulate_oversized_file=false; throw; } simulate_oversized_file=false; status(result,-1,"临时文件超限"); check(read(f.file())==original,"超限时必须保留原文件"); });
    run("超限条数失败必须填满输出，避免末位假成功", [] { Fixture f; const int count=2000001; Series out(count,1); float code=600000,market=1,period=5; Func4(count,out.data(),&code,&market,&period); check(std::all_of(out.begin(),out.end(),[](float value){return value==-1;}),"不能只改首位而保留末位成功状态"); check(!fs::exists(f.file()),"超限不得落盘"); });
    run("旧 CSV 日期倒序不得掩盖历史冲突", [] { Fixture f; auto original=header()+row("1d","2026-10-16 15:00:00")+row("1d","2026-10-15 15:00:00"); write(f.file(),original); Batch b; status(b.dates({1261019}),1,"日期"); b.data(); status(b.save(),-1,"旧文件倒序"); check(read(f.file())==original,"倒序文件必须原样保留"); });
    const std::vector<std::string> corrupt = {
        header()+"坏行\n", header()+"600000.SH,1d,2026-10-15 15:00:00,oops,12,9,11,100,1000\n",
        header()+"600000.SH,1d,2026-02-30 15:00:00,10,12,9,11,100,1000\n",
        header()+"600000.SH,1d,2026-10-15 15:00:00,nan,12,9,11,100,1000\n",
        header()+"600001.SH,1d,2026-10-15 15:00:00,10,12,9,11,100,1000\n",
        header()+"600000.SH,1d,2026-10-15 15:00:00,10,8,9,11,100,1000\n"
    };
    for(size_t i=0;i<corrupt.size();++i) run("旧文件损坏保护 " + std::to_string(i+1), [=] { Fixture f; write(f.file(),corrupt[i]); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"坏文件保存"); check(read(f.file())==corrupt[i],"旧文件必须逐字节保留"); });
    for(bool nulInHeader : {false,true}) run(nulInHeader?"旧表头内含 NUL 必须拒绝":"旧数据行内含 NUL 必须拒绝",[=] { Fixture f; std::string original; if(nulInHeader) original=header().substr(0,header().size()-1)+std::string(1,'\0')+"被截断的坏表头\n"+row("1d","2026-10-15 15:00:00"); else original=header()+row("1d","2026-10-15 15:00:00").substr(0,row("1d","2026-10-15 15:00:00").size()-1)+std::string(1,'\0')+"被截断的坏内容\n"; write(f.file(),original); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); status(b.save(),-1,"含 NUL 的旧文件"); check(read(f.file())==original,"含 NUL 的旧文件必须逐字节保留"); });
    run("旧文件读失败不得覆盖", [] { Fixture f; auto original=header()+row(); write(f.file(),original); Series result; { FileHandle h(CreateFileA(f.file().string().c_str(),GENERIC_WRITE,FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr)); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); result=b.save(); } status(result,-1,"读失败"); check(read(f.file())==original,"读失败时原文件必须逐字节不变"); });
    run("替换被占用不得截断旧文件", [] { Fixture f; auto original=header()+row(); write(f.file(),original); Series result; { FileHandle h(CreateFileA(f.file().string().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); result=b.save(); } status(result,-1,"替换失败"); check(read(f.file())==original,"替换失败时原文件必须逐字节不变"); });
    run("写锁冲突安全失败且原文件不变", [] { Fixture f; auto original=header()+row(); write(f.file(),original); Series result; { FileHandle lock(CreateFileA((f.file().string()+".lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,nullptr)); Batch b; status(b.dates({1261016}),1,"日期"); b.data(); result=b.save(); } status(result,-1,"写锁冲突"); check(read(f.file())==original,"锁冲突时原文件必须逐字节不变"); });
    struct Snapshot { int code; const char *period; const char *keyName; const char *key; };
    for(auto p : std::vector<Snapshot>{{6,"1w","week_key","2026W42"},{7,"1mo","month_key","202610"},{11,"1y","year_key","2026"}}) run(std::string("较早快照不得回退 ")+p.period,[=] { Fixture f; auto original=header(p.keyName)+row(p.period,"2026-10-16 15:00:00",p.key); write(f.file(p.period),original); Batch b(1,p.code); status(b.dates({1261015}),1,"较早日期"); b.data(); status(b.save(),-1,"回退"); check(read(f.file(p.period))==original,"回退失败应保留旧文件"); });
    run("两个线程的股票和数组相互隔离", [] {
        Fixture f; std::mutex mutex; std::condition_variable changed; int ready=0; std::string errors[2];
        auto task=[&](int index) { try { Batch b(1,5,0,600000+index); { std::unique_lock<std::mutex> lock(mutex); ++ready; changed.notify_all(); changed.wait(lock,[&]{return ready>=2;}); } status(b.dates({1261016}),1,"日期");b.data();status(call(Func1,1,{3},b.previous,{0}),1,"异步接收"); } catch(const std::exception&e) { errors[index]=e.what(); {std::lock_guard<std::mutex> lock(mutex); ready=2; changed.notify_all();} } };
        std::thread a(task,0),b(task,1); a.join();b.join(); check(errors[0].empty()&&errors[1].empty(),errors[0]+errors[1]);status(call(Func7,1,{0},{0},{0}),1,"全部生产线程完成后排空一次");contains(read(f.file()),"600000.SH"); contains(read(f.file("1d","600001.SH")),"600001.SH");
    });
    run("其他线程未开始不能续传本线程导出", [] {
        Fixture f; Batch b; std::string error; std::thread other([&] {try { status(call(Func2,1,{1261016},{0},{1}),-1,"其他线程未开始"); status(call(Func1,1,{3},{1},{0}),-1,"其他线程不能提交"); } catch(const std::exception&e){error=e.what();} }); other.join(); check(error.empty(),error);status(b.dates({1261016}),1,"本线程日期");b.data();status(call(Func1,1,{3},b.previous,{0}),1,"本线程仍可提交");status(call(Func7,1,{0},{0},{0}),-1,"其他线程的拒绝应保留到排空");contains(read(f.file()),",10,12,9,11,");
    });
    run("同一文件并发保存不能互相吞掉成功数据", [] {
        Fixture f; write(f.file(),header()+row("1d","2026-10-14 15:00:00")); std::mutex mutex; std::condition_variable changed; int ready=0; std::string errors[2]; float result[2]={-999,-999};
        auto task=[&](int index) { try { Batch b; status(b.dates({static_cast<float>(1261015+index)}),1,"日期"); b.data(); {std::unique_lock<std::mutex> lock(mutex); ++ready; changed.notify_all(); changed.wait(lock,[&]{return ready>=2;});} auto out=call(Func1,1,{3},b.previous,{0}); check(out[0]==1||out[0]==-1,"竞争提交必须明确成功或失败"); for(float value:out) check(value==out[0],"提交输出必须一致"); result[index]=out[0]; } catch(const std::exception&e) { errors[index]=e.what(); {std::lock_guard<std::mutex> lock(mutex);ready=2;changed.notify_all();} } };
        std::thread a(task,0),b(task,1);a.join();b.join();check(errors[0].empty()&&errors[1].empty(),errors[0]+errors[1]);check(result[0]==1||result[1]==1,"两次无外部锁的提交至少一次应成功");status(call(Func7,1,{0},{0},{0}),result[0]==1&&result[1]==1?1.f:-1.f,"后台保存全部已接收任务");auto csv=read(f.file());contains(csv,"2026-10-14");if(result[0]==1)contains(csv,"2026-10-15");if(result[1]==1)contains(csv,"2026-10-16");check(lines(csv)==2+(result[0]==1?1u:0u)+(result[1]==1?1u:0u),"成功数据必须完整保留，失败数据不得混入");
    });
    run("成功保存输出两条完整性能日志且耗时关系一致", [] {
        Fixture f; reset_logs(); Batch b(3); b.complete({1261014,1261015,1261016});
        check_reports(current_logs(),"ok",3);
        check(lines(read(f.file()))==4,"性能记录不能改变保存内容");
        const auto original=read(f.file());
        for(int count:{1,0}) {
            reset_logs();const auto result=call(Func1,count,{count==0?3.f:0.f},{1},{0});
            if(count>0) status(result,-1,"旧非保存命令仍须拒绝");
            else check(result[0]!=1,"零长度调用不得返回成功");
            for(const auto &entry:current_logs())
                check(entry.message.rfind("导出耗时",0)!=0&&entry.message.rfind("保存分段",0)!=0,"非保存命令或非法长度不得生成上一股票的性能报告");
            check(read(f.file())==original,"被拒绝调用不得改变已保存数据");
        }
    });
    run("读取旧文件失败仍输出性能定位且不进入合并写入", [] {
        Fixture f; const auto original=header()+"坏行\n"; write(f.file(),original);
        reset_logs(); Batch b(3); status(b.dates({1261014,1261015,1261016}),1,"日期"); b.data();
        status(b.save(),-1,"旧文件损坏"); const auto entries=current_logs(); check_reports(entries,"failed",3);
        const auto parts=report_fields(entries,"保存分段");
        check(metric_number(parts,"merge_ms")==0&&metric_number(parts,"write_ms")==0,"读失败后不得合并或写临时文件");
        check(std::any_of(entries.begin(),entries.end(),[](const auto &entry){return entry.level=="ERROR";}),"失败应保留错误日志");
        check(read(f.file())==original,"失败性能日志不能改变原文件");
    });
    for(const char *prefix : {"导出耗时","保存分段"}) run(std::string("性能日志抛异常不改变已提交成功 ")+prefix,[=] {
        Fixture f; PerformanceLogFailure failure(prefix); Batch b(2); b.complete({1261015,1261016});
        check(lines(read(f.file()))==3,"性能日志故障不能丢失已提交数据");
        {std::lock_guard<std::mutex> lock(captured_log::mutex);check(captured_log::thrown>0,"必须实际触发性能日志异常注入");}
        const auto original=read(f.file());status(b.save(),-1,"成功后仍禁止重复保存");check(read(f.file())==original,"重复保存保护不能被日志异常绕过");
    });
    run("无效异步配置经导出边界保留拒绝到排空",[]{
        Fixture f;write(f.dir/"saveAsCVS.ini","[log]\nmode=disable\n[export]\ncsv_dir=csv\n[async]\nqueue_capacity=0\nqueue_memory_mb=1\n");
        status(call(Func4,1,{600000},{1},{5}),-1,"零队列容量配置");
        status(call(Func6,1,{3},{0},{0}),1,"配置错误须可查询");status(call(Func6,1,{3},{0},{0}),1,"查询不得清除错误");
        status(call(Func7,1,{0},{0},{0}),-1,"配置错误须使排空失败");check(!fs::exists(f.file()),"配置错误不得生成CSV");
    });
    run("任务超过字节限额经导出边界保留拒绝",[]{
        Fixture f;write(f.dir/"saveAsCVS.ini","[log]\nmode=disable\n[export]\ncsv_dir=csv\n[async]\nqueue_capacity=1000\nqueue_memory_mb=1\n");
        const int count=40000;status(call(Func4,count,values(count,600000),values(count,1),values(count,5)),-1,"1MiB无法容纳约1.28MiB任务");
        status(call(Func6,1,{3},{0},{0}),1,"过大任务必须记录拒绝");status(call(Func6,1,{5},{0},{0}),0,"拒绝后采集额度必须释放");
        status(call(Func7,1,{0},{0},{0}),-1,"任务超限不能误报排空成功");check(!fs::exists(f.file()),"超限任务不得落盘");
    });
    run("进程地址占用95%仍可完成采集及旧CSV合并",[]{
        Fixture f;InjectedWriter injected(high_address_usage);write(f.file(),header()+row());
        std::atomic<bool> done{false};std::string error;
        struct Producer {std::thread thread;~Producer(){if(thread.joinable()){call(Func8,1,{0},{0},{0});thread.join();}}};
        Producer producer{std::thread([&]{try{Batch b(2);b.complete({1261019,1261020});}catch(const std::exception &e){error=e.what();}done=true;})};
        wait_for_condition([&]{return done.load();},"95%地址占用不应阻塞完整导出");producer.thread.join();check(error.empty(),error);
        const auto csv=read(f.file());check(lines(csv)==4,"高地址占用下须保留旧行并追加两行");
        contains(csv,"2026-10-16");contains(csv,"2026-10-19");contains(csv,"2026-10-20");
        status(call(Func6,1,{3},{0},{0}),0,"正常合并不得产生拒绝或失败");
    });
    for(int stride:{1,2,9})run("前台连续三次真实分配失败后保留任务重试 "+std::to_string(stride),[=]{
        Fixture f;InjectedWriter injected(allocation_memory);AllocationControl faults;
        allocation_fault::stride=stride;allocation_fault::probe_failures=3;
        Batch b(2);b.complete({1261019,1261020});
        check(allocation_fault::triggered==3,"必须实际触发预留或数组申请失败三次");
        check(lines(read(f.file()))==3,"分配恢复后应完整保存两行");
        status(call(Func6,1,{3},{0},{0}),0,"内存重试成功不能算拒绝或失败");
        status(call(Func6,1,{5},{0},{0}),0,"重试不得泄漏采集预留");
    });
    run("前台持续分配失败可取消且旧CSV不变",[]{
        Fixture f;InjectedWriter injected(allocation_memory);AllocationControl faults;const auto original=header()+row();write(f.file(),original);
        allocation_fault::stride=1;allocation_fault::probe_failures=-1;std::atomic<bool> done{false};Series result;
        struct Producer {std::thread thread;~Producer(){if(thread.joinable()){call(Func8,1,{0},{0},{0});thread.join();}}};
        Producer producer{std::thread([&]{result=call(Func4,2,{600000},{1},{5});done=true;})};
        wait_for_condition([]{return allocation_fault::triggered.load()>0&&export_writer->status().waiting>0;},"分配失败应进入可取消等待");
        status(call(Func8,1,{0},{0},{0}),1,"停止接收应取消分配重试");wait_for_condition([&]{return done.load();},"取消未唤醒分配重试");producer.thread.join();
        status(result,-1,"被取消的采集不得伪报成功");check(read(f.file())==original,"取消分配重试不能修改旧CSV");
        status(call(Func7,1,{0},{0},{0}),-1,"取消须保留到排空结果");
    });
    run("最终入队连续分配失败仍保留原数组且只接收保存一次",[]{
        Fixture f;InjectedWriter injected(allocation_memory);AllocationControl faults;
        Batch b(2);status(b.dates({1261019,1261020}),1,"完整日期");b.data();
        allocation_fault::stride=1;allocation_fault::probe_failures=3;status(b.save(),1,"入队重试后保存");
        check(allocation_fault::triggered==3,"最终入队须实际经历三次分配失败");
        const auto s=export_writer->status();check(s.accepted==1&&s.succeeded==1&&s.reserved==0&&s.bytes==0&&!s.failed&&!s.rejected,"入队重试不得重复接收、遗失数组或泄漏额度");
        check(lines(read(f.file()))==3,"最终任务内容应完整");
    });
    run("后台临时文件创建后连续分配失败会清理并重试合并",[]{
        Fixture f;AllocationControl faults;write(f.file(),header()+row());allocation_fault::temp_failures=3;
        Batch b(2);b.complete({1261019,1261020});
        check(allocation_fault::triggered==3,"后台须实际经历三次内存申请失败");
        const auto csv=read(f.file());check(lines(csv)==4,"后台重试后应保留旧行并追加两行");contains(csv,"2026-10-16");contains(csv,"2026-10-20");
        for(const auto &file:fs::directory_iterator(f.dir/"csv"))check(file.path().extension()!=".tmp","失败尝试不能遗留临时文件");
        status(call(Func6,1,{3},{0},{0}),0,"后台内存重试成功不得累计任务失败");
        const auto entries=current_logs();
        check(std::any_of(entries.begin(),entries.end(),[](const auto &entry){return entry.message.find("内存分配恢复 phase=save")!=std::string::npos&&entry.message.find("retries=3")!=std::string::npos;}),"后台重试成功须记录恢复次数");
    });
    for(bool parsing:{false,true})run(parsing?"逐个注入CSV解析分配失败仍能重试得到完整数据":"逐个注入时间格式化分配失败不得生成残缺时间",[=]{
        Fixture f;AllocationControl faults;const auto line=row();
        auto action=[&]{
            if(parsing){CsvRow parsed;return parse_csv_row(line.substr(0,line.size()-1),9,parsed,ExportJob{0,0,150000,false,"600000.SH","1d"});}
            return format_ts(20261020,150000)=="2026-10-20 15:00:00";
        };
        const auto before=allocation_fault::allocations;const bool baseline=action();
        const auto count=allocation_fault::allocations-before;check(baseline&&count>0,"无故障基线应成功且包含真实分配点");
        for(int index=0;index<count;++index){
            const auto triggered=allocation_fault::triggered.load();allocation_fault::fail_after=index;
            const bool ok=retry_allocation(action,"save",false);allocation_fault::fail_after=-1;
            check(ok&&allocation_fault::triggered==triggered+1,"每个分配点失败后均应恢复完整数据");
        }
    });
    run("Func4系统提交额度不足可由Func8取消并使排空失败",[]{
        Fixture f;InjectedWriter injected(constrained_memory);std::atomic<bool> done{false};Series result;
        struct Producer {std::thread thread;~Producer(){if(thread.joinable()){call(Func8,1,{0},{0},{0});thread.join();}}};
        Producer producer{std::thread([&]{const int count=64;result=call(Func4,count,values(count,600000),values(count,1),values(count,5));done=true;})};
        wait_for_condition([]{return export_writer->status().waiting>0;},"Func4未在申请前等待内存");check(!done,"系统提交额度不足不得放行");
        status(call(Func8,1,{0},{0},{0}),1,"停止接收应可解除等待");wait_for_condition([&]{return done.load();},"停止输入未唤醒Func4");producer.thread.join();
        status(result,-1,"取消后Func4必须拒绝");status(call(Func6,1,{3},{0},{0}),1,"取消或拒绝须可查询");
        status(call(Func7,1,{0},{0},{0}),-1,"被取消导出不能误报排空成功");check(!fs::exists(f.file()),"被取消采集不得生成CSV");
    });
    run("排空放弃尚未提交的采集必须报告中止且下一周期可恢复",[]{
        Fixture f;Batch partial(2);status(partial.dates({1261015,1261016}),1,"未提交日期");status(partial.field(1,{10,10}),1,"仅接收开盘");
        status(call(Func6,1,{5},{0},{0}),1,"未提交采集应仍持有预留");
        status(call(Func7,1,{0},{0},{0}),-1,"放弃未提交采集不可误报全部成功");
        status(call(Func6,1,{5},{0},{0}),0,"中止后预留必须归零");status(call(Func6,1,{0},{0},{0}),0,"不完整任务不得进入后台");
        status(call(Func6,1,{4},{0},{0}),0,"中止后任务字节必须归零");
        status(call(Func6,1,{3},{0},{0}),1,"中止必须保留失败标志");status(call(Func6,1,{3},{0},{0}),1,"查询不得清除中止失败");
        check(!fs::exists(f.file()),"不完整采集不得写CSV");
        Batch complete(2);complete.complete({1261019,1261020});
        const auto csv=read(f.file());check(lines(csv)==3,"新周期应只保存完整的新数据");check(csv.find("2026-10-15")==std::string::npos,"已中止旧数据不得混入");
        status(call(Func6,1,{3},{0},{0}),0,"新完整周期成功后应独立报告成功");
    });
}
std::vector<captured_log::Entry> actual_log_entries(const fs::path &directory, const std::string &suffix) {
    std::vector<captured_log::Entry> result;
    for(const auto &file:fs::directory_iterator(directory)) {
        const auto name=file.path().filename().string();
        if(name.size()<suffix.size()||name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0) continue;
        std::istringstream lines(read(file.path()));std::string line;
        while(std::getline(lines,line)) for(const char *prefix:{"导出耗时","保存分段"}) {
            const auto position=line.find(prefix);if(position==std::string::npos)continue;
            check(line.find("[INFO]")!=std::string::npos,"真实性能日志级别应为 INFO");
            result.push_back({std::this_thread::get_id(),"INFO",line.substr(position)});
        }
    }
    return result;
}
void copy_runtime(const fs::path &source,const fs::path &dll) {
    fs::copy_file(source,dll,fs::copy_options::overwrite_existing);
    const auto worker=source.parent_path()/"SaveAsCSVWorker.exe";
    check(fs::exists(worker),"配套的Win32后台缺失");
    fs::copy_file(worker,dll.parent_path()/worker.filename(),fs::copy_options::overwrite_existing);
}
void dll_smoke(const fs::path &dllPath, int logLevel = 1) {
    const fs::path directory=dllPath.parent_path();
    const auto logName="performance-level"+std::to_string(logLevel)+".log";
    const auto ini="[log]\nmode=file\nlevel="+std::to_string(logLevel)+"\nfile="+logName+"\n[export]\ncsv_dir=dll-csv\nsymbol_width=6\ndefault_time=150000\n";
    write(directory/"saveAsCVS.ini",ini);
    fs::create_directories(directory/"dll-csv");
    if(status_files(directory/"dll-csv")==0)write(directory/"dll-csv"/"_bg_previous","");
    HMODULE module=LoadLibraryA(dllPath.string().c_str());
    check(module!=nullptr,"实际发布 DLL 无法被 Win32 进程加载");
    try {
        auto function=[&](const char *name) { auto fn=reinterpret_cast<Function>(GetProcAddress(module,name)); check(fn!=nullptr,std::string("缺少导出函数 ")+name); return fn; };
        auto registerFunctions=reinterpret_cast<BOOL (*)(PluginTCalcFuncInfo **)>(GetProcAddress(module,"RegisterTdxFunc"));
        check(registerFunctions!=nullptr,"缺少 RegisterTdxFunc");
        PluginTCalcFuncInfo *info=nullptr;
        check(registerFunctions(&info)==TRUE&&info!=nullptr,"DLL 函数表注册失败");
        for(int i=0;i<8;++i) check(info[i].nFuncMark==i+1&&info[i].pCallFunc!=nullptr,"DLL 函数表缺失");
        auto previous=call(function("Func4"),1,{600000},{1},{5});status(previous,1,"DLL 开始导出");
        previous=call(function("Func5"),1,previous,{0},{0});status(previous,1,"DLL 设置");
        previous=call(function("Func2"),1,{1261016},{0},previous);status(previous,1,"DLL 日期");
        float data[]={0,10,11,12,9,100,1000};
        for(int type=1;type<=6;++type) {previous=call(function("Func3"),1,{static_cast<float>(type)},{data[type]},previous);status(previous,1,"DLL 字段");}
        previous=call(function("Func1"),1,{3},previous,{0});status(previous,1,"DLL 接收任务");
        status(call(function("Func7"),1,{0},{0},{0}),1,"DLL 排空后确认保存");
        check(fs::exists(directory/"dll-csv"/"_bg_0-1000_0-8")&&status_files(directory/"dll-csv")==1&&fs::file_size(directory/"dll-csv"/"_bg_0-1000_0-8")==0,"真实DLL排空须等待状态线程发布唯一空文件后退出");
        const auto csv=read(directory/"dll-csv"/"600000.SH.1d.csv");
        contains(csv,"2026-10-16 15:00:00");contains(csv,",10,12,9,11,100,1000");
        check(csv.find('+')==std::string::npos,"DLL 时间戳不应包含时区");
    } catch(...) { FreeLibrary(module);throw; }
    FreeLibrary(module);
    check(read(directory/"saveAsCVS.ini")==ini,"既有日志配置不得被自动覆盖");
    const auto entries=actual_log_entries(directory,logName);
    if(logLevel==1) {
        check_reports(entries,"ok",1);
        std::string startup_logs;
        for(const auto &file:fs::directory_iterator(directory)) {
            const auto name=file.path().filename().string();
            if(name.size()>=logName.size()&&name.compare(name.size()-logName.size(),logName.size(),logName)==0)startup_logs+=read(file.path());
        }
        for(const char *marker:{"DLL接口注册成功","通达信首次进入导出接口","首次导出配置读取完成","准备启动独立后台","独立后台进程已创建"})contains(startup_logs,marker);
    }
    else check(entries.empty(),"关闭 INFO 后不应输出两类性能日志，保存仍应成功");
}
void dll_existing_threads_smoke(const fs::path &dllPath) {
    std::mutex mutex; std::condition_variable changed; int ready=0; bool start=false;
    HMODULE module=nullptr; Function functions[5]{}; std::string loadError, errors[2];
    auto worker=[&](int index) {
        try {
            { std::unique_lock<std::mutex> lock(mutex); ++ready; changed.notify_all(); changed.wait(lock,[&]{return start;}); }
            check(loadError.empty(),loadError);
            auto previous=call(functions[3],1,{static_cast<float>(600001+index)},{1},{5});
            status(previous,1,"已有工作线程开始导出");
            previous=call(functions[4],1,previous,{0},{0});status(previous,1,"工作线程设置");
            previous=call(functions[1],1,{1261016},{0},previous);status(previous,1,"工作线程日期");
            float data[]={0,10,11,12,9,100,1000};
            for(int type=1;type<=6;++type) {previous=call(functions[2],1,{static_cast<float>(type)},{data[type]},previous);status(previous,1,"工作线程字段");}
            previous=call(functions[0],1,{3},previous,{0});status(previous,1,"工作线程提交");
        } catch(const std::exception &e) { errors[index]=e.what(); }
    };
    std::thread first(worker,0),second(worker,1);
    { std::unique_lock<std::mutex> lock(mutex); changed.wait(lock,[&]{return ready==2;}); }
    module=LoadLibraryA(dllPath.string().c_str());
    if(!module) loadError="工作线程已存在时无法加载 DLL";
    else for(int i=0;i<5;++i) {
        functions[i]=reinterpret_cast<Function>(GetProcAddress(module,("Func"+std::to_string(i+1)).c_str()));
        if(!functions[i]) loadError="DLL 缺少工作线程调用函数";
    }
    { std::lock_guard<std::mutex> lock(mutex); start=true; changed.notify_all(); }
    first.join();second.join();
    if(module) {
        auto finish=reinterpret_cast<Function>(GetProcAddress(module,"Func7"));
        if(!finish)loadError="DLL 缺少排空管理函数";
        else if(call(finish,1,{0},{0},{0})[0]!=1)loadError="已有工作线程提交后排空失败";
        FreeLibrary(module);
    }
    check(loadError.empty()&&errors[0].empty()&&errors[1].empty(),loadError+errors[0]+errors[1]);
    contains(read(dllPath.parent_path()/"dll-csv"/"600001.SH.1d.csv"),"600001.SH,1d,2026-10-16 15:00:00");
    contains(read(dllPath.parent_path()/"dll-csv"/"600002.SH.1d.csv"),"600002.SH,1d,2026-10-16 15:00:00");
}
Function dll_function(HMODULE module,const char *name) {
    const auto function=reinterpret_cast<Function>(GetProcAddress(module,name));
    check(function!=nullptr,std::string("生命周期验证缺少函数 ")+name);return function;
}
void dll_submit_one(HMODULE module,int code,int date,int period=5,int time=0) {
    auto previous=call(dll_function(module,"Func4"),1,{static_cast<float>(code)},{1},{static_cast<float>(period)});status(previous,1,"生命周期开始");
    previous=call(dll_function(module,"Func5"),1,previous,{0},{0});status(previous,1,"生命周期设置");
    previous=call(dll_function(module,"Func2"),1,{static_cast<float>(date)},{static_cast<float>(time)},previous);status(previous,1,"生命周期日期");
    float data[]={0,10,11,12,9,100,1000};
    for(int type=1;type<=6;++type){previous=call(dll_function(module,"Func3"),1,{static_cast<float>(type)},{data[type]},previous);status(previous,1,"生命周期数值");}
    status(call(dll_function(module,"Func1"),1,{3},previous,{0}),1,"生命周期任务提交");
}
void dll_period_15m(const fs::path &source) {
    const auto dir=source.parent_path()/"period-15m";fs::create_directories(dir);
    const auto dll=dir/"period15_async.dll";copy_runtime(source,dll);
    write(dir/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=period15.log\n[export]\ncsv_dir=csv\n");
    HMODULE host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"15分钟DLL加载失败");
    try {
        for(int time:{930,945,945,1000})dll_submit_one(host,600150,1261016,2,time);
        status(call(dll_function(host,"Func7"),1,{0},{0},{0}),1,"15分钟全部排空");
        const auto csv=read(dir/"csv"/"600150.SH.15m.csv");check(lines(csv)==4,"真实DLL必须合并重复时间而保留三个15分钟时间点");
        for(const std::string time:{"09:30:00","09:45:00","10:00:00"})contains(csv,"600150.SH,15m,2026-10-16 "+time+",10,12,9,11,100,1000");
        check(!fs::exists(dir/"csv"/"600150.SH.5m.csv"),"真实DLL不能混写其他周期");
        FreeLibrary(host);host=nullptr;
    }catch(...){if(host){call(dll_function(host,"Func7"),1,{0},{0},{0});FreeLibrary(host);}throw;}
}
void dll_deferred_invalid(const fs::path &source,int kind) {
    const auto dir=source.parent_path()/("raw-invalid-"+std::to_string(kind));fs::create_directories(dir/"csv");
    const auto dll=dir/"raw_invalid.dll";copy_runtime(source,dll);
    write(dir/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=raw.log\n[export]\ncsv_dir=csv\n");
    const bool weekly=kind<2;const std::string period=weekly?"1w":"15m";
    const auto file=dir/"csv"/("600000.SH."+period+".csv");
    const auto original=header()+row(period,"2026-10-14 09:30:00");write(file,original);
    HMODULE host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"原始输入DLL加载失败");
    try {
        const int n=kind==5?2:1;const float nan=std::numeric_limits<float>::quiet_NaN();
        auto previous=call(dll_function(host,"Func4"),n,values(n,600000),values(n,1),values(n,weekly?6.f:2.f));status(previous,1,"原始输入开始");
        previous=call(dll_function(host,"Func5"),n,previous,values(n,0),values(n,0));status(previous,1,"原始输入设置");
        const Series dates=kind==5?Series{1261016,1261015}:Series{kind==0?nan:(kind==1?1261331.f:1261016.f)};
        previous=call(dll_function(host,"Func2"),n,dates,values(n,kind==2?std::numeric_limits<float>::infinity():930.f),previous);status(previous,1,"非法内容只做安全复制");
        for(int type=1;type<=6;++type){float v[]={0,10,11,12,9,100,1000};float input=v[type];if(kind==3&&type==5)input=-1;if(kind==4&&type==3)input=std::numeric_limits<float>::infinity();previous=call(dll_function(host,"Func3"),n,values(n,static_cast<float>(type)),values(n,input),previous);status(previous,1,"原始数值复制");}
        status(call(dll_function(host,"Func1"),n,values(n,3),previous,values(n,0)),1,"非法内容任务已入队");
        dll_submit_one(host,600001,1261019); // 错误任务不得阻止后续股票。
        status(call(dll_function(host,"Func7"),1,{0},{0},{0}),-1,"后台校验失败必须汇总");
        status(call(dll_function(host,"Func6"),1,{3},{0},{0}),1,"后台错误标记保留");
        status(call(dll_function(host,"Func6"),1,{0},{0},{0}),0,"全部任务已经处理结束");
        check(read(file)==original,"后台校验失败不能覆盖旧文件");
        contains(read(dir/"csv"/"600001.SH.1d.csv"),"600001.SH,1d,2026-10-19 15:00:00");
        FreeLibrary(host);host=nullptr;
    }catch(...){if(host){call(dll_function(host,"Func7"),1,{0},{0},{0});FreeLibrary(host);}throw;}
}
void dll_pool_bulk(const fs::path &source,int workers,bool damaged=false) {
    const auto dir=source.parent_path()/("pool-"+std::to_string(workers)+(damaged?"-damaged":"-ok"));
    fs::create_directories(dir/"csv");const auto dll=dir/"pool_async.dll";
    copy_runtime(source,dll);
    write(dir/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=pool.log\n[export]\ncsv_dir=csv\n[async]\nqueue_capacity=16\nworker_threads="+std::to_string(workers)+"\n");
    if(damaged)write(dir/"csv"/"600100.SH.1d.csv","故意损坏的原文件，必须原样保留\n");
    HMODULE host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"并行集成DLL加载失败");
    try {
        for(int pass=0;pass<8;++pass)for(int stock=0;stock<12;++stock)dll_submit_one(host,600100+stock,1261001+pass);
        status(call(dll_function(host,"Func7"),1,{0},{0},{0}),damaged?-1.0f:1.0f,"并行混合文件排空");
        status(call(dll_function(host,"Func6"),1,{0},{0},{0}),0,"全部并行任务完成");
        status(call(dll_function(host,"Func6"),1,{2},{0},{0}),0,"全部工作线程停止保存");
        status(call(dll_function(host,"Func6"),1,{3},{0},{0}),damaged?1.0f:0.0f,"最终错误必须保留");
        for(int stock=0;stock<12;++stock) {
            const auto symbol=std::to_string(600100+stock)+".SH";
            const auto content=read(dir/"csv"/(symbol+".1d.csv"));
            if(damaged&&stock==0){check(content=="故意损坏的原文件，必须原样保留\n","后台错误不得覆盖旧文件");continue;}
            check(lines(content)==9,"每个文件必须保留八次提交的全部日期");
            for(int pass=0;pass<8;++pass)contains(content,symbol+",1d,2026-10-0"+std::to_string(pass+1)+" 15:00:00,10,12,9,11,100,1000");
        }
        FreeLibrary(host);host=nullptr;
        check(GetModuleHandleA(dll.filename().string().c_str())==nullptr,"并行排空后不能残留线程保护引用");
        contains(read(dir/"saveAsCVS.ini"),"worker_threads="+std::to_string(workers));
    } catch(...) {if(host){call(dll_function(host,"Func7"),1,{0},{0},{0});FreeLibrary(host);}throw;}
}
void dll_lifecycle(const fs::path &source) {
    const auto dir=source.parent_path()/"module-lifecycle";fs::create_directories(dir);
    const auto dll=dir/"lifecycle_async.dll";copy_runtime(source,dll);
    write(dir/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=lifecycle.log\n[export]\ncsv_dir=csv\n");
    HMODULE host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"生命周期DLL加载失败");
    try {
        dll_submit_one(host,600003,1261016);
        check(FreeLibrary(host)!=FALSE,"释放宿主引用失败");host=nullptr;
        check(GetModuleHandleA(dll.filename().string().c_str())==nullptr,"独立后台保存不能阻止接收DLL卸载");
        // 必须先重新取得真实加载引用，才能调用排空；不使用已释放的裸函数指针。
        host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"重新取得宿主模块引用失败");
        status(call(dll_function(host,"Func7"),1,{0},{0},{0}),1,"重新加载后排空");
        check(FreeLibrary(host)!=FALSE,"正常排空后卸载失败");host=nullptr;
        check(GetModuleHandleA(dll.filename().string().c_str())==nullptr,"排空并释放宿主引用后DLL应彻底卸载");
        for(int iteration=0;iteration<3;++iteration) {
            host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"重复加载失败");
            dll_submit_one(host,600003,1261019+iteration);
            status(call(dll_function(host,"Func7"),1,{0},{0},{0}),1,"重复加载后排空");
            check(FreeLibrary(host)!=FALSE,"重复加载后卸载失败");host=nullptr;
            check(GetModuleHandleA(dll.filename().string().c_str())==nullptr,"重复加载排空后不应残留模块引用");
        }
        check(lines(read(dir/"csv"/"600003.SH.1d.csv"))==5,"各次生命周期的已提交数据必须完整保留");
    } catch(...) {
        if(!host&&GetModuleHandleA(dll.filename().string().c_str()))host=LoadLibraryA(dll.string().c_str());
        if(host){const auto finish=reinterpret_cast<Function>(GetProcAddress(host,"Func7"));if(finish)call(finish,1,{0},{0},{0});FreeLibrary(host);}
        throw;
    }
}
int dll_exit_child(const fs::path &dll) {
    try {
        write(dll.parent_path()/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=child.log\n[export]\ncsv_dir=csv\n");
        const auto host=LoadLibraryA(dll.string().c_str());check(host!=nullptr,"子进程DLL加载失败");
        // 接收多个任务后直接从 main 正常退出，故意不调用排空或 FreeLibrary。
        for(int index=0;index<8;++index)dll_submit_one(host,600010+index,1261019);
        return 0;
    } catch(...) {return 3;}
}
void dll_process_exit(const fs::path &source) {
    const auto dir=source.parent_path()/"exit-child";fs::create_directories(dir);
    const auto dll=dir/"exit_async.dll";copy_runtime(source,dll);
    wchar_t executable[MAX_PATH]{};check(GetModuleFileNameW(nullptr,executable,MAX_PATH)>0,"无法找到当前Win32测试进程");
    const std::wstring arguments=L"\""+std::wstring(executable)+L"\" --async-exit-child \""+dll.wstring()+L"\"";
    std::vector<wchar_t> command(arguments.begin(),arguments.end());command.push_back(L'\0');
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,dir.c_str(),&startup,&process)!=FALSE,"无法启动独立退出测试子进程");
    const auto waited=WaitForSingleObject(process.hProcess,15000); // 仅防止退出死锁，不作性能阈值。
    if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,4);WaitForSingleObject(process.hProcess,5000);}
    DWORD exit_code=0;GetExitCodeProcess(process.hProcess,&exit_code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    check(waited==WAIT_OBJECT_0,"未排空的独立子进程在退出时锁死");check(exit_code==0,"未排空退出发生异常，退出码 "+std::to_string(exit_code));
}
} // namespace test

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if(argc==3&&std::string(argv[1])=="--async-exit-child")return test::dll_exit_child(argv[2]);
    if(argc!=2&&argc!=3) { std::cerr << "需要本次运行的临时目录，可另指定实际 DLL 进行加载验证\n"; return 2; }
    test::root=std::filesystem::path(argv[1])/"samples";
    std::filesystem::create_directories(test::root);
    if(argc==3) {
        test::run("实际 Win32 发布 DLL 注册保存及 INFO 性能日志",[&]{test::dll_smoke(argv[2]);});
        test::run("已有工作线程动态加载 DLL 后相互隔离",[&]{test::dll_existing_threads_smoke(argv[2]);});
        test::run("实际日志关闭 INFO 后保持正常保存",[&]{test::dll_smoke(argv[2],2);});
        test::run("接收DLL卸载后独立后台保留任务及重新绑定排空",[&]{test::dll_lifecycle(argv[2]);});
        test::run("独立进程未排空退出不会锁死",[&]{test::dll_process_exit(argv[2]);});
        for(int workers:{2,8,32,128})test::run("实际DLL按"+std::to_string(workers)+"线程混合保存12个文件96个任务",[&]{test::dll_pool_bulk(argv[2],workers);});
        test::run("实际DLL并行保存时损坏文件保持且其他文件完整",[&]{test::dll_pool_bulk(argv[2],8,true);});
        test::run("实际DLL支持15分钟同日合并与去重",[&]{test::dll_period_15m(argv[2]);});
        for(int kind=0;kind<6;++kind)test::run("实际DLL后台拒绝非法原始输入并继续后续任务 "+std::to_string(kind),[&]{test::dll_deferred_invalid(argv[2],kind);});
    }
    else test::suite();
    std::cout << "合计：" << test::passed << " 通过，" << test::failed << " 失败\n";
    return test::failed==0?0:1;
}
