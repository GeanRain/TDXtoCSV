// Win32 独立保存进程：接收与管理分离；宿主退出即终止，不等待剩余保存。
#include "../shared/ipc.h"
#include "../shared/csv_store.h"
#include "../SaveAsCSV/common.h"
#include "../SaveAsCSV/utils/ini_reader.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <shellapi.h>
#include <thread>
using namespace tdx_export;
using namespace tdx_export::ipc;
#ifdef TDX_WORKER_TEST
bool test_save(ExportJob &);
bool test_drop_reply(Command);
#endif
const char *kDefaultIni="[log]\nmode=file\nlevel=1\nfile=saveAsCSV.log\n[async]\nqueue_capacity=1000\nqueue_memory_mb=2048\nworker_threads=8\n";
namespace
{
std::uint64_t number(const std::wstring &text)
{
    if(text.empty()||text.size()>20)throw std::runtime_error("后台启动参数无效");
    std::uint64_t n=0;for(auto c:text){if(c<L'0'||c>L'9'||n>(UINT64_MAX-(c-L'0'))/10)throw std::runtime_error("后台启动数值无效");n=n*10+c-L'0';}return n;
}
class ParentLifetime
{
    Handle parent,stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::thread watcher;
public:
    ParentLifetime(DWORD pid,std::uint64_t created):parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid))
    {
        if(!parent||creation_time(parent.value)!=created)throw std::runtime_error("原宿主进程不存在或已更换");
        if(!stop)win_error("创建宿主监视停止事件");
        watcher=std::thread([this]{
            HANDLE events[]={parent.value,stop.value};
            // 不取日志/任务锁，不等待保存线程或内存重试；只有主动排空才走正常清理。
            if(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0)
                TerminateProcess(GetCurrentProcess(),ERROR_PROCESS_ABORTED);
        });
    }
    ~ParentLifetime(){SetEvent(stop.value);if(watcher.joinable())watcher.join();}
};
struct Incoming
{
    AsyncWriter &writer;std::uint64_t id,bytes,connection,request;ExportJob job;
    std::atomic<bool> ready{false},cancelled{false};bool accepted=false;
    std::uint32_t crc=0;unsigned next_field=0;std::size_t offset=0;
    Incoming(AsyncWriter &w,std::uint64_t token,std::uint64_t size,std::uint64_t owner,std::uint64_t request_id):writer(w),id(token),bytes(size),connection(owner),request(request_id){}
    ~Incoming(){if(!accepted)writer.release(id);}
};
class Server
{
#ifdef TDX_WORKER_TEST
    AsyncWriter writer{test_save};
#else
    AsyncWriter writer{perform_save};
#endif
    AsyncLimits limits;
    Handle parent,stop_control{CreateEventW(nullptr,TRUE,FALSE,nullptr)},stop_data{CreateEventW(nullptr,TRUE,FALSE,nullptr)},
        drain{CreateEventW(nullptr,TRUE,FALSE,nullptr)},final_ack{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    Channel control,data;
    std::mutex mutex;std::map<std::uint64_t,std::shared_ptr<Incoming>> tasks;
    std::atomic<bool> stopping{false},done{false};std::atomic<std::uint64_t> rejected{0};
    const DWORD parent_pid;const std::uint64_t parent_created,epoch;
    std::uint64_t connection=0;
    WriterStatus snapshot(){auto s=writer.status();s.rejected+=rejected.load();return s;}
    void acknowledge(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);auto i=tasks.find(id);if(i!=tasks.end()&&i->second->accepted)tasks.erase(i);
    }
    void release_pending()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for(auto i=tasks.begin();i!=tasks.end();)if(!i->second->accepted){i->second->cancelled=true;writer.note_rejected();i=tasks.erase(i);}else ++i;
    }
    void verify_peer(Channel &channel)
    {
        ULONG pid=0;if(!GetNamedPipeClientProcessId(channel.handle(),&pid)||pid!=parent_pid)throw std::runtime_error("拒绝非宿主的管道连接");
    }
    Packet reply(const Packet &request,Result status)
    {
        Packet p;p.command=request.command;p.id=request.id;p.epoch=epoch;p.body.u32(static_cast<unsigned>(status));return p;
    }
    void request_drain()
    {
        std::lock_guard<std::mutex> lock(mutex);stopping=true;writer.stop_input();SetEvent(drain.value);
    }
    Packet command(const Packet &request)
    {
        Reader r(request.body.data);
        if(request.command==Command::Hello){
            const auto pid=r.u32();const auto created=r.u64();const auto cp=r.u32();const auto resume=r.u64();const auto ack=r.u64();r.end();
            if(pid!=parent_pid||created!=parent_created||cp!=GetACP()||(request.epoch&&request.epoch!=epoch))throw std::runtime_error("管道握手身份或编码不匹配");
            if(resume&&resume==connection)acknowledge(ack);
            if(!resume||resume!=connection){release_pending();{std::lock_guard<std::mutex> lock(mutex);tasks.clear();}++connection;if(!connection)throw std::runtime_error("连接编号耗尽");}
            auto p=reply(request,Result::Ok);p.body.u32(GetCurrentProcessId());p.body.u32(32);p.body.u32(GetACP());p.body.u64(connection);p.body.str(writer.cancel_event_name());return p;
        }
        if(!connection||request.epoch!=epoch)throw std::runtime_error("后台实例不匹配");
        const auto local_errors=r.u64();const auto ack=r.u64();acknowledge(ack);rejected=std::max(rejected.load(),local_errors);
        if(request.command==Command::Query){r.end();auto p=reply(request,done?Result::Done:Result::Ok);encode_status(p.body,snapshot());return p;}
        if(request.command==Command::Drain){r.end();request_drain();return reply(request,done?Result::Done:Result::Wait);}
        if(request.command==Command::FinalAck){r.end();if(!done)throw std::runtime_error("后台尚未排空");SetEvent(final_ack.value);return reply(request,Result::Done);}
        if(request.command==Command::Cancel){r.end();writer.cancel_input();release_pending();return reply(request,Result::Ok);}
        if(request.command==Command::Start){
            AsyncLimits requested;requested.capacity=r.u32();requested.bytes=r.u64();requested.workers=r.u32();const auto directory=r.str(MAX_PATH-1);r.end();
            if(stopping)throw std::runtime_error("后台正在排空");
            if(requested.capacity!=limits.capacity||requested.bytes!=limits.bytes||requested.workers!=limits.workers)throw std::runtime_error("后台额度配置已变化，请先排空");
            writer.start(limits,directory);return reply(request,Result::Ok);
        }
        if(request.command==Command::Reserve){
            const auto bytes=r.u64();r.end();if(stopping)throw std::runtime_error("后台停止接收");
            std::lock_guard<std::mutex> lock(mutex);
            if(!request.id)throw std::runtime_error("预留请求缺少稳定编号");
            for(const auto &entry:tasks)if(entry.second->connection==connection&&entry.second->request==request.id){
                if(entry.second->bytes!=bytes)throw std::runtime_error("预留编号被不同内容复用");
                auto p=reply(request,Result::Ok);p.body.u64(entry.first);return p;
            }
            const auto token=writer.try_reserve(bytes);
            if(!token)return reply(request,Result::Wait);
            try{tasks.emplace(token,std::make_shared<Incoming>(writer,token,bytes,connection,request.id));}catch(...){writer.release(token);throw;}
            auto p=reply(request,Result::Ok);p.body.u64(token);return p;
        }
        std::lock_guard<std::mutex> lock(mutex);
        auto found=tasks.find(request.id);
        if(request.command==Command::Release){r.end();if(found!=tasks.end()&&!found->second->accepted){found->second->cancelled=true;tasks.erase(found);}return reply(request,Result::Ok);}
        if(request.command==Command::Receipt){r.end();if(found!=tasks.end()&&found->second->accepted)tasks.erase(found);return reply(request,Result::Ok);}
        if(found==tasks.end())throw std::runtime_error("预留或待确认任务已经失效");
        auto &item=*found->second;
        if(request.command==Command::TaskStatus){const auto checksum=r.u32();r.end();if((item.accepted||item.ready)&&item.crc!=checksum)throw std::runtime_error("同一任务号内容不一致");return reply(request,item.accepted?Result::Accepted:item.ready?Result::Ready:Result::Wait);}
        if(request.command==Command::Commit){
            const auto checksum=r.u32();r.end();
            if(!item.accepted&&!item.ready)return reply(request,Result::Wait);
            if(item.crc!=checksum)throw std::runtime_error("提交摘要不一致");
            if(item.accepted)return reply(request,Result::Accepted);
            if(stopping||item.cancelled)throw std::runtime_error("任务已中止");
            if(job_memory(item.job)>item.bytes)throw std::runtime_error("实际任务超过原预留额度");
            writer.submit(item.id,std::move(item.job));item.accepted=true;return reply(request,Result::Accepted);
        }
        throw std::runtime_error("未知管道管理命令");
    }
    void control_loop() noexcept
    {
        while(WaitForSingleObject(stop_control.value,0)==WAIT_TIMEOUT&&WaitForSingleObject(parent.value,0)==WAIT_TIMEOUT){
            try{
                control.accept(stop_control.value,parent.value);verify_peer(control);
                for(;;){
                    auto request=control.receive(control_limit,stop_control.value,parent.value,0);
                    Packet response;
                    try{response=command(request);}catch(const std::bad_alloc &){response=reply(request,Result::Wait);}
                    catch(const std::exception &error){response=reply(request,Result::Rejected);response.body.str(error.what());}
#ifdef TDX_WORKER_TEST
                    if(test_drop_reply(request.command))throw std::runtime_error("测试注入：命令完成后丢失回包");
#endif
                    control.send(response,stop_control.value,parent.value);
                }
            }catch(const std::exception &error){if(!stopping)try{LOG_WARN("后台控制连接中断：%s",error.what());}catch(...){};}
            catch(...){try{LOG_ERROR("%s","后台控制连接异常");}catch(...){};}
            DisconnectNamedPipe(control.handle());
        }
    }
    void data_loop() noexcept
    {
        while(WaitForSingleObject(stop_data.value,0)==WAIT_TIMEOUT&&WaitForSingleObject(parent.value,0)==WAIT_TIMEOUT){
            std::shared_ptr<Incoming> current;
            try{
                data.accept(stop_data.value,parent.value);verify_peer(data);
                auto hello=data.receive(control_limit,stop_data.value,parent.value);Reader hr(hello.body.data);const auto owner=hr.u64();hr.end();
                {std::lock_guard<std::mutex> lock(mutex);if(hello.command!=Command::Hello||hello.epoch!=epoch||owner!=connection)throw std::runtime_error("数据连接身份不匹配");}
                data.send(reply(hello,Result::Ok),stop_data.value,parent.value);
                for(;;){
                    auto packet=data.receive(data_limit,stop_data.value,parent.value,0);
                    if(packet.epoch!=epoch)throw std::runtime_error("数据包后台实例错误");
                    if(packet.command==Command::Begin){
                        {std::lock_guard<std::mutex> lock(mutex);auto i=tasks.find(packet.id);if(i==tasks.end()||i->second->accepted||i->second->cancelled||i->second->connection!=owner)throw std::runtime_error("数据接收预留无效");current=i->second;current->ready=false;}
                        auto next_report=GetTickCount64();
                        for(;;){
                            if(stopping||current->cancelled||WaitForSingleObject(parent.value,0)!=WAIT_TIMEOUT)throw std::runtime_error("后台接收已停止");
                            try{
                                Reader meta(packet.body.data);auto job=decode_job(meta);meta.end();job.id=current->id;
                                for(unsigned field=0;field<8;++field)if(field!=1||job.minute)array(job,field).resize(job.count);
                                if(job_memory(job)>current->bytes)throw std::runtime_error("接收数组超过预留额度");
                                current->job=std::move(job);break;
                            }catch(const std::bad_alloc &){if(GetTickCount64()>=next_report){next_report=GetTickCount64()+5000;try{LOG_WARN("后台接收内存分配重试 id=%llu retry_ms=200",current->id);}catch(...){};}WaitForSingleObject(stop_data.value,200);}
                        }
                        current->crc=crc32(0,packet.body.data.data(),packet.body.data.size());current->next_field=0;current->offset=0;
                        data.send(reply(packet,Result::Ready),stop_data.value,parent.value);continue;
                    }
                    if(!current||current->id!=packet.id||current->cancelled||current->ready)throw std::runtime_error("数据接收阶段错误");
                    Reader r(packet.body.data);
                    if(packet.command==Command::Data){
                        const auto field=r.u32(),offset=r.u32();const auto bytes=packet.body.data.size()-r.at;
                        if(field!=current->next_field||offset!=current->offset||field>=8||!bytes||bytes%4)throw std::runtime_error("数据数组顺序或偏移错误");
                        auto &values=array(current->job,field);if(offset>values.size()||bytes/4>values.size()-offset)throw std::runtime_error("数据数组超长");
                        std::memcpy(values.data()+offset,packet.body.data.data()+r.at,bytes);current->crc=crc32(current->crc,packet.body.data.data()+r.at,bytes);current->offset+=bytes/4;
                        if(current->offset==values.size()){++current->next_field;current->offset=0;if(current->next_field==1&&!current->job.minute)++current->next_field;}
                    }else if(packet.command==Command::DataEnd){
                        const auto checksum=r.u32();const auto elapsed=r.real();r.end();if(current->next_field!=8||checksum!=current->crc||!std::isfinite(elapsed)||elapsed<0)throw std::runtime_error("完整数据长度、CRC32或计时不符");
                        current->job.frontend_elapsed_ms=elapsed;current->job.started=ExportClock::now();
                        current->ready=true;data.send(reply(packet,Result::Ready),stop_data.value,parent.value);current.reset();
                    }else throw std::runtime_error("未知数据管道命令");
                }
            }catch(const std::exception &error){if(!stopping)try{LOG_WARN("后台数据连接中断：%s",error.what());}catch(...){};}
            catch(...){try{LOG_ERROR("%s","后台数据连接异常");}catch(...){};}
            {std::lock_guard<std::mutex> lock(mutex);if(current&&!current->accepted){current->ready=false;current->job=ExportJob{};}}
            current.reset();DisconnectNamedPipe(data.handle());
        }
    }
public:
    Server(DWORD pid,std::uint64_t created,const std::wstring &directory):
        parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)),parent_pid(pid),parent_created(created),
        epoch(creation_time(GetCurrentProcess())^(std::uint64_t(GetCurrentProcessId())<<32))
    {
        if(!parent||creation_time(parent.value)!=created)throw std::runtime_error("原宿主进程不存在或已更换");
        if(!stop_control||!stop_data||!drain||!final_ack)win_error("创建后台生命周期事件");
        IniReader ini(join_path(narrow(directory),INIREADER_FILENAME),kDefaultIni);
        limits.capacity=static_cast<std::size_t>(number(wide(ini.getString("async","queue_capacity","1000"))));
        const auto megabytes=number(wide(ini.getString("async","queue_memory_mb","2048")));
        const auto workers=number(wide(ini.getString("async","worker_threads","8")));
        if(!limits.capacity||limits.capacity>INT_MAX||!megabytes||megabytes>INT_MAX||!workers||workers>kMaxWorkers)throw std::runtime_error("后台队列参数无效");
        limits.bytes=megabytes*1024*1024;limits.workers=static_cast<std::size_t>(workers);
        const auto base=pipe_base(pid,created,directory);auto cp=create_pipe(base+L".control"),dp=create_pipe(base+L".data");
        control=Channel(std::exchange(cp.value,nullptr));data=Channel(std::exchange(dp.value,nullptr));writer.start(limits);
    }
    int run()
    {
        std::thread controls,receiver;
        try{
            controls=std::thread([&]{control_loop();});receiver=std::thread([&]{data_loop();});
            WaitForSingleObject(drain.value,INFINITE);
            request_drain();SetEvent(stop_data.value);CancelIoEx(data.handle(),nullptr);receiver.join();release_pending();
            writer.finish();done=true;const auto s=snapshot();
            LOG_INFO("后台排空完成 accepted=%llu succeeded=%llu failed=%llu rejected=%llu remaining=%llu",s.accepted,s.succeeded,s.failed,s.rejected,s.unfinished);
            WaitForSingleObject(final_ack.value,INFINITE);
        }catch(...){SetEvent(stop_control.value);SetEvent(stop_data.value);CancelIoEx(control.handle(),nullptr);CancelIoEx(data.handle(),nullptr);if(receiver.joinable())receiver.join();if(controls.joinable())controls.join();throw;}
        SetEvent(stop_control.value);CancelIoEx(control.handle(),nullptr);controls.join();return 0;
    }
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
    bool logging=false;
    try{
        int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);if(!arguments)win_error("解析后台启动参数");
        std::vector<std::wstring> args;for(int i=0;i<count;++i)args.emplace_back(arguments[i]);LocalFree(arguments);
        if(args.size()!=5||args[1]!=L"--parent")return 2;
        const auto pid=number(args[2]),created=number(args[3]);if(!pid||pid>MAXDWORD)return 2;
        ParentLifetime lifetime(static_cast<DWORD>(pid),created);
        Logger::initialize(narrow(args[4]),"worker_"+std::to_string(GetCurrentProcessId())+"_");logging=true;
        const auto base=pipe_base(static_cast<DWORD>(pid),created,args[4]);Handle instance(CreateMutexW(nullptr,FALSE,(L"Local\\"+base.substr(9)).c_str()));
        if(!instance)win_error("创建后台实例锁");if(GetLastError()==ERROR_ALREADY_EXISTS){Logger::shutdown();return 0;}
        LOG_INFO("独立后台启动 pid=%lu parent=%llu architecture=x86 protocol=%u",GetCurrentProcessId(),pid,version);
        Server server(static_cast<DWORD>(pid),created,args[4]);const auto result=server.run();Logger::shutdown();return result;
    }catch(const std::exception &error){if(logging)try{LOG_ERROR("后台致命错误：%s",error.what());}catch(...){};}
    catch(...){if(logging)try{LOG_ERROR("%s","后台致命异常");}catch(...){};}
    if(logging)Logger::shutdown();return 3;
}
