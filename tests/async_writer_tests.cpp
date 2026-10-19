// 异步保存队列回归：使用受条件变量控制的保存器和可控进程内存探针。
#include "../SaveAsCSV/export/async_writer.h"
#include "../SaveAsCSV/utils/logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <process.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace allocation_fault {
thread_local bool fail_next=false;
thread_local bool triggered=false;
}
namespace thread_fault {
std::atomic<int> fail_after{-1}, attempts{0}, allocation_at{-1};
unsigned (__stdcall *allocation_entry)(void *)=nullptr;
unsigned __stdcall with_allocation_failure(void *context) {
    allocation_fault::fail_next=true;
    return allocation_entry(context);
}
}
uintptr_t __cdecl test_beginthreadex(void *security,unsigned stack_size,
    unsigned (__stdcall *entry)(void *),void *context,unsigned flags,unsigned *id) {
    const int attempt=thread_fault::attempts.fetch_add(1);
    if(thread_fault::fail_after.load()>=0&&attempt==thread_fault::fail_after.load())return 0;
    if(attempt==thread_fault::allocation_at.load()) {
        thread_fault::allocation_entry=entry;
        return _beginthreadex(security,stack_size,thread_fault::with_allocation_failure,context,flags,id);
    }
    return _beginthreadex(security,stack_size,entry,context,flags,id);
}
// 仅测试构建替换线程启动边界，生产 DLL 不增加测试开关。
#define _beginthreadex test_beginthreadex
#include "../SaveAsCSV/export/async_writer.cpp"
#undef _beginthreadex

// 仅当前测试线程下一次申请失败，随后自动恢复；不靠耗尽进程内存制造故障。
void *operator new(std::size_t bytes) {
    if(allocation_fault::fail_next) {
        allocation_fault::fail_next=false;allocation_fault::triggered=true;throw std::bad_alloc();
    }
    if(void *memory=std::malloc(bytes?bytes:1))return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void *memory) noexcept {std::free(memory);}
void operator delete[](void *memory) noexcept {std::free(memory);}
void operator delete(void *memory,std::size_t) noexcept {std::free(memory);}
void operator delete[](void *memory,std::size_t) noexcept {std::free(memory);}

namespace warning_log {
std::mutex mutex;
std::condition_variable changed;
bool blocked=false;
std::atomic<bool> entered{false};
std::vector<std::string> entries;
void release() {std::lock_guard<std::mutex> lock(mutex);blocked=false;changed.notify_all();}
}
void Logger::log(const char *level, const char *format, ...) {
    char buffer[4096]{};va_list arguments;va_start(arguments,format);std::vsnprintf(buffer,sizeof(buffer),format,arguments);va_end(arguments);
    std::unique_lock<std::mutex> lock(warning_log::mutex);
    warning_log::entries.emplace_back(buffer);
    if(std::string(level)=="WARN")warning_log::entered=true;
    if(std::string(level)=="WARN"&&warning_log::blocked) {
        warning_log::entered=true;
        warning_log::changed.wait(lock,[]{return !warning_log::blocked;});
    }
}

namespace test {
using namespace tdx_export;
std::mutex notification_mutex;
std::condition_variable changed;
std::atomic<uint64_t> virtual_total{1ULL << 40}, virtual_available{1ULL << 40}, commit_available{1ULL << 40};
std::atomic<bool> probe_valid{true};
int passed=0, failed=0;

void check(bool condition,const std::string &message) { if(!condition) throw std::runtime_error(message); }
void rejects(const std::function<void()> &action,const std::string &message) {
    bool rejected=false;try{action();}catch(const std::exception &){rejected=true;}
    check(rejected,message);
}
bool reserve_rejected(AsyncWriter &writer,uint64_t bytes) {
    // 模拟 DLL 边界：当前采集首次失败时统一计一次拒绝，队列本身只抛异常。
    try{writer.reserve(bytes);return false;}catch(const std::runtime_error &){writer.note_rejected();return true;}
}
bool probe(uint64_t &total,uint64_t &available,uint64_t &commit) {
    total=virtual_total.load();available=virtual_available.load();commit=commit_available.load();
    changed.notify_all();return probe_valid.load();
}
void wait_until(const std::function<bool()> &ready,const std::string &message) {
    // 超时只用于结束死锁用例，不用实际耗时作为性能验收条件。
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    std::unique_lock<std::mutex> lock(notification_mutex);
    while(!ready()) {
        check(std::chrono::steady_clock::now()<deadline,message);
        changed.wait_for(lock,std::chrono::milliseconds(20));
    }
}
bool logged(const std::string &prefix,const std::string &part) {
    std::lock_guard<std::mutex> lock(warning_log::mutex);
    for(const auto &entry:warning_log::entries)if(entry.rfind(prefix,0)==0&&entry.find(part)!=std::string::npos)return true;
    return false;
}
ExportJob job(const std::string &symbol="600000.SH",int date=20261019) {
    ExportJob result;result.symbol=symbol;result.period="1d";result.csv_dir="任务自带目录";
    result.count=1;result.native_period=5;result.default_time=150000;result.minute=false;
    result.dates={static_cast<float>(date-19000000)};result.times={1500};
    float values[]={10,11,12,9,100,1000};
    for(int i=0;i<6;++i)result.series[i]={values[i]};
    return result;
}
struct Observed { std::string symbol,dir;int date;float open;const float *address; };
struct SaverControl {
    std::mutex mutex;std::condition_variable condition;
    bool released=false;size_t permits=0;bool fail_first=false,throw_first=false;
    std::vector<Observed> saved;
    bool save(ExportJob &item) {
        std::unique_lock<std::mutex> lock(mutex);const auto index=saved.size();
        saved.push_back({item.symbol,item.csv_dir,static_cast<int>(item.dates.at(0))+19000000,item.series[0].at(0),item.series[0].data()});
        changed.notify_all();condition.wait(lock,[&]{return released||permits>0;});
        if(!released)--permits;
        if(index==0&&throw_first)throw std::runtime_error("测试注入：单个保存任务抛异常");
        return !(index==0&&fail_first);
    }
    void one() { std::lock_guard<std::mutex> lock(mutex);++permits;condition.notify_all(); }
    void all() { std::lock_guard<std::mutex> lock(mutex);released=true;condition.notify_all(); }
    size_t started() { std::lock_guard<std::mutex> lock(mutex);return saved.size(); }
    std::vector<Observed> snapshot() { std::lock_guard<std::mutex> lock(mutex);return saved; }
};
SaverControl *active_saver=nullptr;
bool save(ExportJob &item) { return active_saver->save(item); }

struct Fixture {
    SaverControl saver;AsyncWriter writer{&save,&probe};
    std::vector<std::thread> threads;std::mutex error_mutex;std::exception_ptr error;
    Fixture() {
        warning_log::release();warning_log::entered=false;
        {std::lock_guard<std::mutex> lock(warning_log::mutex);warning_log::entries.clear();}
        probe_valid=true;virtual_total=1ULL<<40;virtual_available=1ULL<<40;commit_available=1ULL<<40;
        active_saver=&saver;
    }
    ~Fixture() {
        thread_fault::fail_after=-1;
        thread_fault::allocation_at=-1;
        warning_log::release();
        writer.cancel_input();saver.all();
        for(auto &thread:threads)if(thread.joinable())thread.join();
        writer.finish();active_saver=nullptr;
    }
    void start(size_t capacity,uint64_t bytes,size_t workers=1) { AsyncLimits limits;limits.capacity=capacity;limits.bytes=bytes;limits.workers=workers;writer.start(limits);check(writer.status().running,"启动后没有保存线程"); }
    void spawn(std::function<void()> action) {
        threads.emplace_back([&,action]{try{action();}catch(...){std::lock_guard<std::mutex> lock(error_mutex);error=std::current_exception();}changed.notify_all();});
    }
    void join() {
        for(auto &thread:threads)if(thread.joinable())thread.join();
        if(error)std::rethrow_exception(error);
    }
    void submit(ExportJob item) {
        const auto id=writer.reserve(job_memory(item));check(id!=0,"有效任务预留失败");
        writer.submit(id,std::move(item));
    }
};
void run(const std::string &name,const std::function<void()> &action) {
    try {action();++passed;std::cout<<"通过 "<<name<<'\n';}
    catch(const std::exception &error){++failed;std::cout<<"失败 "<<name<<"："<<error.what()<<'\n';}
}

void suite() {
    run("管道非阻塞预留满时返回等待且释放后可接收",[]{
        Fixture f;const auto bytes=job_memory(job());f.start(1,bytes*2);f.submit(job());
        wait_until([&]{return f.saver.started()==1;},"后台任务没有开始");
        check(f.writer.try_reserve(bytes)==0,"满额的非阻塞预留不能占住管理线程");
        f.saver.all();wait_until([&]{return f.writer.status().unfinished==0;},"后台未释放任务");
        const auto id=f.writer.try_reserve(bytes);check(id!=0,"释放后非阻塞预留应成功");f.writer.release(id);
        f.writer.stop_input();check(f.writer.finish(),"宿主结束停止接收不能凭空增加拒绝计数");
    });
    run("状态线程快照分配失败后重试且继续刷新归零",[]{
        Fixture f;
        const auto directory=std::filesystem::current_path()/("tdx-status-memory-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        std::filesystem::create_directories(directory);AsyncLimits limits;limits.workers=1;
        thread_fault::attempts=0;thread_fault::allocation_at=1;f.writer.start(limits,directory.string());
        wait_until([&]{return std::filesystem::exists(directory/"_bg_0-1000_0-1");},"状态快照申请失败后线程没有恢复");
        check(logged("后台状态内存不足","retry_ms=200"),"状态线程应报告分配失败重试");
        f.submit(job());wait_until([&]{return std::filesystem::exists(directory/"_bg_0-1000_1-1");},"恢复后不能继续发布正在保存状态");
        f.saver.all();check(f.writer.finish(),"状态分配重试不能影响CSV任务结果");
        check(std::filesystem::exists(directory/"_bg_0-1000_0-1")&&f.writer.status().succeeded==1,"状态线程应正常发布最终归零并停机");
    });
    run("队列已空但系统提交额度不足仍等待且恢复后自动继续",[]{
        Fixture f;auto item=job();const auto bytes=job_memory(item);f.start(2,bytes*4);f.submit(std::move(item));
        wait_until([&]{return f.saver.started()==1;},"首个任务未开始");virtual_total=1000000;virtual_available=50000;commit_available=bytes-1;
        std::atomic<bool> done{false};uint64_t next=0;f.spawn([&]{next=f.writer.reserve(bytes);done=true;});
        wait_until([&]{return f.writer.status().waiting==1&&warning_log::entered.load();},"系统提交额度不足应等待");f.saver.one();
        wait_until([&]{return f.writer.status().unfinished==0;},"后台未排空");check(!done,"仅队列排空不得绕过系统提交额度检查");
        wait_until([&]{return logged("等待导出资源","blocked_by=commit_limit pending=0 reserved=0 busy=0");},"等待超过五秒时未刷新队列已空及实际等待原因");
        const bool reason=logged("等待导出资源","blocked_by=commit_limit")&&logged("等待导出资源","virtual_used=950000");
        commit_available=1ULL<<40; // 地址占用仍为95%，不发队列通知，由定时复查恢复。
        wait_until([&]{return done.load();},"内存恢复后未自动继续预留");f.join();f.writer.release(next);
        check(reason,"等待日志必须明确系统提交额度及当前占用");check(logged("导出资源恢复","phase=reserve blocked_by=commit_limit"),"恢复接收必须记录之前的等待原因");
        check(f.writer.finish(),"内存恢复后的轮次应成功");
    });
    run("系统可提交内存不足会独立报告并在恢复后继续",[]{
        Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*4);commit_available=bytes-1;
        std::atomic<bool> done{false};uint64_t next=0;f.spawn([&]{next=f.writer.reserve(bytes);done=true;});
        wait_until([&]{return f.writer.status().waiting==1&&warning_log::entered.load();},"系统提交额度不足应等待");const bool reason=logged("等待导出资源","blocked_by=commit_limit");
        commit_available=1ULL<<40;wait_until([&]{return done.load();},"系统提交额度恢复后未自动继续");f.join();f.writer.release(next);
        check(reason&&logged("导出资源恢复","phase=reserve blocked_by=commit_limit"),"系统提交额度不得误报为队列满或地址空间90%");check(f.writer.finish(),"提交额度恢复后应成功");
    });
    run("最终提交不因进程地址占用95%等待且仅接收一次",[]{
        Fixture f;auto item=job();const auto bytes=job_memory(item);f.start(2,bytes*4);const auto id=f.writer.reserve(bytes);
        virtual_total=1000000;virtual_available=50000;std::atomic<bool> done{false};f.spawn([&]{f.writer.submit(id,std::move(item));done=true;});
        wait_until([&]{return done.load();},"95%地址占用不应阻塞最终提交");f.join();
        const auto s=f.writer.status();check(s.reserved==0&&s.accepted==1&&s.unfinished==1&&s.waiting==0,"提交须把预留转成唯一任务");
        f.saver.all();check(f.writer.finish(),"高地址占用下提交仍应成功保存");check(f.writer.status().succeeded==1,"任务应保存一次");
    });
    for(int successful:{0,3,8,31,32,127,128})run((successful==8||successful==32||successful==128?"启动状态线程失败（"+std::to_string(successful)+"线程）":"启动第"+std::to_string(successful+1)+"个保存线程失败")+std::string("应回收并允许重启"),[=]{
        Fixture f;AsyncLimits limits;limits.workers=successful>32?128:successful>8?32:8;thread_fault::attempts=0;thread_fault::fail_after=successful;
        rejects([&]{f.writer.start(limits);},"线程未全部创建时不得报告启动成功");
        thread_fault::fail_after=-1;
        auto s=f.writer.status();check(thread_fault::attempts==successful+1&&!s.running&&!s.accepting&&!s.busy&&!s.unfinished&&!s.bytes,"部分启动失败不能留下运行线程或任务额度");
        f.writer.note_rejected();check(!f.writer.finish(),"启动失败应由边界保留到排空");
        f.writer.start(limits);f.submit(job());f.saver.all();check(f.writer.finish(),"回收后应允许重新启动配置的全部线程");
        check(f.writer.status().succeeded==1,"重启后任务必须保存");
    });
    run("线程数0和129均拒绝且运行中变更必须先排空",[]{
        Fixture f;AsyncLimits limits;limits.workers=0;rejects([&]{f.writer.start(limits);},"0线程不得启动");
        limits.workers=129;rejects([&]{f.writer.start(limits);},"超过128线程不得启动");
        limits.workers=2;f.writer.start(limits);limits.workers=3;
        rejects([&]{f.writer.start(limits);},"运行中修改线程数必须拒绝");
        check(f.writer.status().accepting&&f.writer.status().running,"拒绝变更不能中止原后台");
        check(f.writer.finish(),"原后台应正常结束");f.writer.start(limits);check(f.writer.finish(),"排空后可采用新线程数");
    });
    run("默认八线程同时保存且第九个任务等待空闲线程",[]{
        Fixture f;f.writer.start();check(AsyncLimits{}.workers==8,"默认并行数必须为8");
        for(int i=0;i<10;++i)f.submit(job("60000"+std::to_string(i)+".SH"));
        wait_until([&]{return f.saver.started()==8;},"未能同时开始八个独立文件任务");
        auto s=f.writer.status();check(s.busy==8&&s.unfinished==10&&s.accepted==10,"同时保存数和未完成数不一致");
        check(f.saver.started()==8,"并发任务不能超过默认8个");
        f.saver.all();check(f.writer.finish(),"十个任务应全部排空成功");
        s=f.writer.status();check(s.succeeded==10&&!s.busy&&!s.running&&!s.bytes,"全部线程退出后额度应归零");
    });
    run("配置两个线程同时工作且排空等待所有线程",[]{
        Fixture f;f.start(8,100000,2);
        for(int i=0;i<4;++i)f.submit(job("60000"+std::to_string(i)+".SH"));
        wait_until([&]{return f.saver.started()==2;},"配置2个线程未生效");
        check(f.writer.status().busy==2&&f.writer.status().unfinished==4,"工作线程必须受配置上限约束");
        std::atomic<bool> done{false};bool result=false;
        f.spawn([&]{result=f.writer.finish();done=true;});
        wait_until([&]{return !f.writer.status().accepting;},"排空未停止接收");check(!done,"仍有两个线程保存时不得提前返回");
        f.saver.all();wait_until([&]{return done.load();},"全部任务完成后排空未结束");f.join();
        check(result&&f.writer.status().succeeded==4&&!f.writer.status().running,"必须等待全部工作线程退出");
    });
    for(int workers:{9,32,128})run("配置"+std::to_string(workers)+"线程同时保存且额外任务等待",[=]{
        Fixture f;f.start(workers+1,100000,workers);
        for(int i=0;i<=workers;++i)f.submit(job(std::to_string(600000+i)+".SH"));
        wait_until([&]{return f.saver.started()==static_cast<size_t>(workers);},"配置的保存线程未全部开始");
        const auto count=static_cast<uint64_t>(workers);const auto s=f.writer.status();check(s.busy==count&&s.unfinished==count+1&&s.accepted==count+1,"并行数和等待任务统计错误");
        f.saver.all();check(f.writer.finish(),"全部任务应排空");
        const auto done=f.writer.status();check(done.succeeded==count+1&&!done.busy&&!done.unfinished&&!done.bytes&&!done.running,"排空应等待所有线程退出并归还额度");
    });
    for(int workers:{8,32,128})run("配置"+std::to_string(workers)+"线程时同文件串行且其他文件可继续",[=]{
        Fixture f;AsyncLimits limits;limits.workers=workers;f.writer.start(limits);f.submit(job("600000.SH",20261019));
        wait_until([&]{return f.saver.started()==1;},"首个同文件任务未开始");
        f.submit(job("600000.SH",20261020));f.submit(job("600001.SH",20261021));
        wait_until([&]{return f.saver.started()==2;},"同文件等待任务阻挡了其他文件");
        auto saved=f.saver.snapshot();check(saved[0].symbol=="600000.SH"&&saved[1].symbol=="600001.SH","同文件的第二个任务不能同时执行");
        check(f.writer.status().busy==2&&f.writer.status().unfinished==3,"同文件等待不能被误计为正在保存");
        f.saver.all();check(f.writer.finish(),"按文件串行后应全部成功");saved=f.saver.snapshot();
        check(saved.size()==3&&saved[2].symbol=="600000.SH"&&saved[2].date==20261020,"同一文件必须按提交顺序处理");
    });
    run("目录大小写和分隔符不同的同一目标仍须串行",[]{
        Fixture f;f.writer.start();auto first=job();first.csv_dir="C:\\Data\\CSV";f.submit(std::move(first));
        wait_until([&]{return f.saver.started()==1;},"首个文件未开始");
        auto second=job("600000.SH",20261020);second.csv_dir="c:/data/csv/";f.submit(std::move(second));
        f.submit(job("600001.SH"));wait_until([&]{return f.saver.started()==2;},"其他目标应并行");
        auto saved=f.saver.snapshot();check(saved[1].symbol=="600001.SH","同一路径大小写差异不能绕过串行保护");
        f.saver.all();check(f.writer.finish(),"同一路径的任务应全部保存");
    });
    for(bool throwing:{false,true})run(throwing?"多线程同文件首项抛异常后后续项仍可保存":"多线程同文件首项失败后后续项仍可保存",[=]{
        Fixture f;f.saver.throw_first=throwing;f.saver.fail_first=!throwing;f.writer.start();
        f.submit(job());wait_until([&]{return f.saver.started()==1;},"首个任务未开始");
        for(int i=0;i<5;++i)f.submit(job("600000.SH",20261020+i));
        f.submit(job("600001.SH"));wait_until([&]{return f.saver.started()==2;},"其他文件不能被同文件队列阻塞");
        f.saver.all();check(!f.writer.finish(),"首个任务失败必须体现在汇总");
        const auto s=f.writer.status();check(s.accepted==7&&s.failed==1&&s.succeeded==6&&!s.unfinished&&!s.bytes&&!s.busy,"失败后应释放该文件并继续所有任务");
        const auto saved=f.saver.snapshot();int last=20261019;
        for(const auto &item:saved)if(item.symbol=="600000.SH"){check(item.date>=last,"同文件后续任务发生乱序");last=item.date;}
    });
    run("默认额度为1000个任务和2048MiB",[]{AsyncLimits limits;check(limits.capacity==1000&&limits.bytes==2048ULL*1024*1024,"默认资源限制不一致");});
    run("预留不算提交且释放后额度归零",[]{Fixture f;auto item=job();const auto bytes=job_memory(item);f.start(2,bytes*2);const auto id=f.writer.reserve(bytes);check(id!=0,"预留失败");auto s=f.writer.status();check(s.reserved==1&&s.unfinished==0&&s.accepted==0&&s.bytes==bytes,"采集预留与已接收任务必须分开");f.writer.release(id);f.writer.release(id);s=f.writer.status();check(s.reserved==0&&s.bytes==0,"释放预留必须幂等且不能减成负数");check(f.writer.finish(),"纯释放不应算失败");});
    for(bool byte_limit:{false,true})run(byte_limit?"正在保存仍占字节额度且释放后唤醒":"正在保存仍占数量额度且释放后唤醒",[=]{
        Fixture f;auto first=job();const auto bytes=job_memory(first);f.start(byte_limit?2:1,byte_limit?bytes:bytes*4);f.submit(std::move(first));
        wait_until([&]{return f.saver.started()==1;},"保存器未开始");auto s=f.writer.status();check(s.busy&&s.unfinished==1&&s.bytes==bytes&&s.reserved==0,"出队不得提前释放正在保存的数据额度");
        virtual_total=1000000;virtual_available=50000;
        std::atomic<bool> done{false};uint64_t next=0;f.spawn([&]{next=f.writer.reserve(bytes);done=true;});
        wait_until([&]{return f.writer.status().waiting>0&&warning_log::entered.load();},"额度满后没有进入等待");check(!done,"额度满不能伪造预留成功");
        const bool reason=logged("等待导出资源",byte_limit?"blocked_by=queue_bytes":"blocked_by=queue_count");
        f.saver.one();wait_until([&]{return done.load();},"保存完成后未唤醒资源等待");f.join();check(next!=0,"资源恢复后应预留成功");f.writer.release(next);check(f.writer.finish(),"正常资源等待不应被计为失败");
        check(reason&&logged("导出资源恢复",byte_limit?"phase=reserve blocked_by=queue_bytes":"phase=reserve blocked_by=queue_count"),"数量或字节等待及其恢复必须可区分");
        s=f.writer.status();check(s.succeeded==1&&s.unfinished==0&&s.reserved==0&&s.bytes==0&&!s.busy&&!s.running,"排空应释放全部资源并停止");
    });
    run("预留本身占数量额度且归还后可继续",[]{Fixture f;const auto bytes=job_memory(job());f.start(1,bytes*2);const auto first=f.writer.reserve(bytes);check(first!=0,"首次预留失败");std::atomic<bool> done{false};uint64_t second=0;f.spawn([&]{second=f.writer.reserve(bytes);done=true;});wait_until([&]{return f.writer.status().waiting>0;},"已有采集预留未限制数量");check(!done,"预留不可绕过容量");f.writer.release(first);wait_until([&]{return done.load();},"归还采集预留未唤醒");f.join();check(second!=0,"归还后预留应成功");f.writer.release(second);check(f.writer.finish(),"正常采集等待不应失败");});
    for(uint64_t additional:{100,200})run(additional==100?"预计恰好90%仍允许尝试申请":"预计超过90%仍允许尝试申请",[=]{Fixture f;f.start(2,100000);virtual_total=10000;virtual_available=1100;std::atomic<bool> done{false};f.spawn([&]{f.writer.check_memory(additional);done=true;});wait_until([&]{return done.load();},"已取消的90%门槛不应阻塞申请");f.join();check(f.writer.status().waiting==0,"高地址占用不应留下等待计数");check(f.writer.finish(),"正常申请检查不应产生失败统计");});
    run("取消唤醒容量等待并停止新增输入",[]{Fixture f;const auto bytes=job_memory(job());f.start(1,bytes*2);f.submit(job());wait_until([&]{return f.saver.started()==1;},"保存器未开始");std::atomic<bool> done{false};uint64_t id=1;f.spawn([&]{try{id=f.writer.reserve(bytes);}catch(...){id=0;}done=true;});wait_until([&]{return f.writer.status().waiting>0;},"预留未等待");f.writer.cancel_input();wait_until([&]{return done.load();},"取消未唤醒容量等待");f.join();check(id==0&&!f.writer.status().accepting,"取消后不得继续预留");f.saver.all();check(!f.writer.finish(),"被拒绝输入必须体现在排空结果");check(f.writer.status().succeeded==1,"取消输入不得丢弃已经接收的任务");});
    run("过大任务立即拒绝且失败查询不会重置",[]{Fixture f;const auto bytes=job_memory(job());f.start(2,bytes);check(reserve_rejected(f.writer,bytes+1),"不可能容纳的任务不能永久等待");const auto a=f.writer.status(),b=f.writer.status();check(a.rejected==1&&b.rejected==1&&a.waiting==0&&a.reserved==0&&a.bytes==0,"拒绝必须保留统计且不占额度");check(!f.writer.finish(),"有拒绝的轮次不能返回成功");});
    run("FIFO及数组所有权转移不受前台重置影响",[]{Fixture f;const auto bytes=job_memory(job());f.start(3,bytes*4);auto first=job("600000.SH",20261019);const auto address=first.series[0].data();const auto id=f.writer.reserve(job_memory(first));check(id!=0,"首个任务预留失败");f.writer.submit(id,std::move(first));first=job("被覆盖的前台",19990101);f.submit(job("600001.SH",20261020));f.submit(job("600002.SH",20261021));f.saver.all();check(f.writer.finish(),"FIFO任务应全部成功");const auto saved=f.saver.snapshot();check(saved.size()==3&&saved[0].symbol=="600000.SH"&&saved[1].symbol=="600001.SH"&&saved[2].symbol=="600002.SH","后台必须按接收顺序保存");check(saved[0].date==20261019&&saved[0].open==10&&saved[0].dir=="任务自带目录"&&saved[0].address==address,"提交必须转移数组所有权并固定任务配置");});
    for(bool throwing:{false,true})run(throwing?"单个保存异常被接住并继续后续任务":"单个保存返回失败仍继续后续任务",[=]{Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*3);f.saver.fail_first=!throwing;f.saver.throw_first=throwing;f.submit(job());f.submit(job("600001.SH"));f.saver.all();check(!f.writer.finish(),"单任务失败必须使排空结果失败");const auto a=f.writer.status(),b=f.writer.status();check(a.accepted==2&&a.failed==1&&a.succeeded==1&&a.unfinished==0&&a.bytes==0&&b.failed==1,"失败不能丢掉后续任务或被查询清除");});
    run("排空不会因等待队列为空而提前返回",[]{Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*2);f.submit(job());wait_until([&]{return f.saver.started()==1;},"保存器未开始");std::atomic<bool> done{false};bool result=false;f.spawn([&]{result=f.writer.finish();done=true;});wait_until([&]{return !f.writer.status().accepting;},"排空应先关闭接收");check(!done&&f.writer.status().unfinished==1&&f.writer.status().busy,"后台仍在写时不能报告排空完成");f.saver.all();wait_until([&]{return done.load();},"任务完成后排空未退出");f.join();check(result&&!f.writer.status().running,"成功排空须等待线程退出");});
    run("停止后重启建立新统计周期",[]{Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*2);f.writer.note_rejected();check(!f.writer.finish(),"采集失败应保留到排空");f.start(2,bytes*2);auto s=f.writer.status();check(s.accepted==0&&s.failed==0&&s.rejected==0&&s.running&&s.accepting,"明确重启后才能重置统计");f.submit(job());f.saver.all();check(f.writer.finish(),"重启轮次应独立成功");check(f.writer.status().succeeded==1,"重启后计数错误");});
    run("旧资源等待者跨停机重启不能减坏新周期等待计数",[]{
        Fixture f;const auto bytes=job_memory(job());f.start(1,bytes*2);const auto held=f.writer.reserve(bytes);check(held!=0,"首次采集预留失败");
        {std::lock_guard<std::mutex> lock(warning_log::mutex);warning_log::blocked=true;warning_log::entered=false;}
        std::atomic<bool> done{false};bool rejected=false;
        f.spawn([&]{try{f.writer.reserve(bytes);}catch(const std::runtime_error &){rejected=true;}done=true;});
        wait_until([&]{return warning_log::entered.load();},"旧调用未进入资源等待日志");
        check(f.writer.status().waiting==1,"旧调用应占一个资源等待计数");
        check(f.writer.finish(),"只归还未提交预留时应正常停机");
        f.start(1,bytes*2);warning_log::release();
        wait_until([&]{return done.load();},"旧等待调用未被停止或新周期隔离唤醒");f.join();
        const auto s=f.writer.status();check(rejected,"旧周期调用不得在新周期取得资源");
        check(s.waiting==0&&s.reserved==0&&s.bytes==0&&s.accepting&&s.running,"旧等待者退出不能把新周期等待计数减成UINT64_MAX");
        check(f.writer.finish(),"新周期不应被旧等待者污染");
    });
    run("外部持有旧停止事件句柄不能取消重启的新周期",[]{
        Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*2);
        struct ExternalEvent {HANDLE value;~ExternalEvent(){if(value)CloseHandle(value);}} event{OpenEventA(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,f.writer.cancel_event_name().c_str())};
        check(event.value!=nullptr,"外部工具无法打开本轮停止事件");check(SetEvent(event.value)!=FALSE,"外部工具无法通知停止输入");
        check(reserve_rejected(f.writer,bytes),"旧周期必须响应停止事件");check(!f.writer.finish(),"旧周期取消应报告失败");
        f.start(2,bytes*2);const auto next=f.writer.reserve(bytes);check(next!=0,"仍存活的旧事件句柄不得取消新周期");
        f.writer.release(next);check(f.writer.finish(),"新周期应独立成功");
    });
    run("进程内存探针失败明确拒绝而不永久等待",[]{Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*2);probe_valid=false;check(reserve_rejected(f.writer,bytes),"探针失败不能伪造可用内存");auto s=f.writer.status();check(s.rejected>0&&s.waiting==0&&s.reserved==0&&s.bytes==0,"探针失败应有可靠拒绝状态");check(!f.writer.finish(),"探针失败不能在排空时伪造成功");});
    run("无效配置与整数溢出任务不得启动或预留",[]{Fixture f;AsyncLimits limits;limits.capacity=0;rejects([&]{f.writer.start(limits);},"零容量应拒绝启动");limits.capacity=1;limits.bytes=0;rejects([&]{f.writer.start(limits);},"零字节额度应拒绝启动");f.start(2,job_memory(job())*2);check(reserve_rejected(f.writer,std::numeric_limits<uint64_t>::max()),"最大整数任务不能溢出后通过");check(!f.writer.finish(),"溢出输入应进入拒绝统计");});
    run("预留节点真实分配失败不能泄漏额度",[]{
        Fixture f;const auto bytes=job_memory(job());f.start(2,bytes*2);bool caught=false;
        allocation_fault::triggered=false;allocation_fault::fail_next=true;
        try{f.writer.reserve(bytes);}catch(const std::bad_alloc &){caught=true;}
        allocation_fault::fail_next=false;
        check(caught&&allocation_fault::triggered,"必须实际触发预留分配失败");
        const auto s=f.writer.status();check(s.accepted==0&&s.unfinished==0&&s.reserved==0&&s.bytes==0,"分配失败不能先增加预留或占用字节");
        f.writer.note_rejected();f.submit(job());f.saver.all();check(!f.writer.finish(),"边界记录的分配失败须保留到排空");
        check(f.writer.status().succeeded==1,"分配故障恢复后应继续保存后续任务");
    });
    run("入队真实分配失败保留前台所有权且可释放预留",[]{
        Fixture f;auto item=job();const auto bytes=job_memory(item);f.start(2,bytes*2);const auto id=f.writer.reserve(bytes);
        const auto original=item.series[0].data();bool caught=false;allocation_fault::triggered=false;allocation_fault::fail_next=true;
        try{f.writer.submit(id,std::move(item));}catch(const std::bad_alloc &){caught=true;}
        allocation_fault::fail_next=false;
        check(caught&&allocation_fault::triggered,"必须实际触发入队分配失败");
        const auto s=f.writer.status();check(s.accepted==0&&s.unfinished==0&&s.reserved==1&&s.bytes==bytes,"失败的入队不能计为已接收或丢失预留");
        check(item.symbol=="600000.SH"&&item.dates.at(0)==1261019&&item.series[0].data()==original,"未提交任务仍须由前台持有");
        f.writer.release(id);check(f.writer.status().bytes==0&&f.writer.status().reserved==0,"失败预留应能正常归还");
        f.writer.note_rejected();f.submit(job());f.saver.all();check(!f.writer.finish(),"分配失败不能在排空时误报成功");
        check(f.writer.status().succeeded==1,"入队分配故障恢复后应继续后续任务");
    });
}
}

int main() {
    test::suite();std::cout<<"合计："<<test::passed<<" 通过，"<<test::failed<<" 失败\n";
    return test::failed==0?0:1;
}
