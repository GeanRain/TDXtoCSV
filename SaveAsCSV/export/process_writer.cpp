#include "process_writer.h"
#include "../../shared/ipc.h"
#include "../common.h"
#include "../utils/ini_reader.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <type_traits>
namespace tdx_export
{
using namespace ipc;
namespace {struct Rejected:std::runtime_error {using std::runtime_error::runtime_error;};}
struct ProcessWriter::Impl
{
    std::mutex mutex;
    const std::wstring directory,base;std::wstring executable;
    const HANDLE lifetime_job;const std::uint64_t lifetime;
    const std::uint64_t created=creation_time(GetCurrentProcess());
    Channel control,data;Handle process,stop;
    std::uint64_t epoch=0,connection=0,acknowledged=0;std::atomic<std::uint64_t> rejected{0},waiting{0};
    AsyncLimits limits;WriterStatus cached;
    DWORD backend_pid=0;bool active=false;std::atomic<bool> reported{false},fault{false};std::uint64_t request_serial=0;
    std::string stop_name;
    Impl(const std::string &dir,HANDLE job,std::uint64_t generation):directory(wide(dir)),
        base(pipe_base(GetCurrentProcessId(),creation_time(GetCurrentProcess()),directory,generation)),lifetime_job(job),lifetime(generation)
    {if(bool(job)!=(generation!=0))throw Rejected("后台生命周期句柄与标识不完整");}
    std::wstring configured_executable(const std::string &relative={})
    {
        if(!relative.empty())return relative_executable(directory,relative);
        IniReader ini(narrow(directory)+"\\"+INIREADER_FILENAME,kDefaultIni);
        return relative_executable(directory,ini.getString("async","worker_path","SaveAsCSVWorker.exe"));
    }
    void disconnect(){data.close();control.close();}
    void check_process()
    {
        if(!process)return;
        const auto state=WaitForSingleObject(process.value,0);
        if(state==WAIT_TIMEOUT)return;
        if(state!=WAIT_OBJECT_0)win_error("检查独立后台进程");
        DWORD code=0;if(!GetExitCodeProcess(process.value,&code))win_error("读取后台退出码");
        fault=true;char message[160]{};
        std::snprintf(message,sizeof(message),"独立后台已退出 exit_code=0x%08lX，请检查配套EXE、x86运行库和后台日志",code);
        try{LOG_ERROR("%s path=%s",message,narrow(executable).c_str());}catch(...){}
        throw Rejected(message);
    }
    void input_ready()
    {
        if(stop&&WaitForSingleObject(stop.value,0)==WAIT_OBJECT_0)throw Rejected("导出已停止接收");
        check_process();
    }
    void pause()
    {
        input_ready();if(stop)WaitForSingleObject(stop.value,200);else Sleep(200);input_ready();
    }
    void verify_server(Channel &channel)
    {
        ULONG pid=0;if(!GetNamedPipeServerProcessId(channel.handle(),&pid))win_error("查询管道后台进程");
        Handle peer(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!peer)win_error("打开独立后台进程");
        wchar_t image[MAX_PATH]{};DWORD size=MAX_PATH;
        if(!QueryFullProcessImageNameW(peer.value,0,image,&size)||CompareStringOrdinal(image,static_cast<int>(size),executable.c_str(),static_cast<int>(executable.size()),TRUE)!=CSTR_EQUAL)
            throw Rejected("管道后台程序路径不匹配");
        if(backend_pid&&backend_pid!=pid)throw Rejected("后台进程已经更换，不能继续旧任务");
        backend_pid=pid;process=std::move(peer);
    }
    Channel open_pipe(const std::wstring &name,bool launch,bool discovery)
    {
        const auto begin=GetTickCount64();bool started=false;
        for(;;){
            Handle pipe(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
            if(pipe)return Channel(std::exchange(pipe.value,nullptr));
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PIPE_BUSY){SetLastError(error);win_error("连接本机保存管道");}
            // 启动期间 epoch 尚未建立，后台也可能已因加载失败退出，不能继续空等。
            check_process();
            if(discovery&&error==ERROR_FILE_NOT_FOUND&&!epoch)return {};
            if(error==ERROR_FILE_NOT_FOUND&&launch&&!started&&!epoch){
                auto arguments=L"\""+executable+L"\" --parent "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(created)+L" \""+directory+L"\"";
                if(lifetime)arguments+=L" "+std::to_wstring(lifetime);
                std::vector<wchar_t> command(arguments.begin(),arguments.end());command.push_back(0);
                STARTUPINFOEXW si{};si.StartupInfo.cb=lifetime_job?sizeof(si):sizeof(si.StartupInfo);PROCESS_INFORMATION pi{};
                std::vector<unsigned char> storage;
                HANDLE job_attribute=lifetime_job;
                std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,decltype(&DeleteProcThreadAttributeList)> attributes(nullptr,DeleteProcThreadAttributeList);
                if(lifetime_job){
                    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);if(!size)win_error("读取后台进程属性长度");
                    storage.resize(size);auto list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                    if(!InitializeProcThreadAttributeList(list,1,0,&size))win_error("初始化后台进程属性");
                    attributes.reset(list);
                    // 创建与入作业必须是同一次系统调用，避免宿主退出留下未管理的后台。
                    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&job_attribute,sizeof(job_attribute),nullptr,nullptr))win_error("设置后台生命周期属性，需要Windows 10或更新版本");
                    si.lpAttributeList=list;
                }
                try{LOG_INFO("准备启动独立后台 path=%s",narrow(executable).c_str());}catch(...){}
                if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|(lifetime_job?EXTENDED_STARTUPINFO_PRESENT:0),nullptr,directory.c_str(),&si.StartupInfo,&pi)){
                    const auto launch_error=GetLastError();
                    try{LOG_ERROR("创建独立后台失败 Windows=%lu path=%s",launch_error,narrow(executable).c_str());}catch(...){}
                    SetLastError(launch_error);win_error("启动独立保存程序");
                }
                Handle thread(pi.hThread);process.reset(pi.hProcess);
                started=true;
                try{LOG_INFO("独立后台进程已创建 pid=%lu，等待管道握手",pi.dwProcessId);}catch(...){}
            }
            if(GetTickCount64()-begin>10000)throw Rejected("独立后台启动或管道连接超时");
            check_process();
            if(stop&&WaitForSingleObject(stop.value,0)==WAIT_OBJECT_0&&launch)throw Rejected("已取消后台启动");
            WaitNamedPipeW(name.c_str(),50);Sleep(10);
        }
    }
    Result result(const Packet &p,Command command,std::uint64_t id)
    {
        if(p.command!=command||p.id!=id||p.epoch!=epoch)throw Rejected("后台响应的任务或实例不匹配");
        Reader r(p.body.data);const auto value=r.u32();
        if(value>static_cast<unsigned>(Result::Done))throw Rejected("后台响应状态无效");
        if(static_cast<Result>(value)==Result::Rejected)throw Rejected(r.str());
        return static_cast<Result>(value);
    }
    bool connect(bool launch)
    {
        if(control)return true;
        if(executable.empty())executable=configured_executable();
        control=open_pipe(base+L".control",launch,!launch);if(!control)return false;
        try{
            verify_server(control);Packet hello;hello.command=Command::Hello;hello.epoch=epoch;
            hello.body.u32(GetCurrentProcessId());hello.body.u64(created);hello.body.u32(GetACP());hello.body.u64(connection);hello.body.u64(acknowledged);
            control.send(hello,nullptr,process.value);auto response=control.receive(control_limit,nullptr,process.value);
            if(epoch&&epoch!=response.epoch)throw Rejected("后台实例已更换");epoch=response.epoch;
            if(result(response,Command::Hello,0)!=Result::Ok)throw Rejected("后台握手未完成");
            Reader r(response.body.data);r.u32();const auto pid=r.u32(),bits=r.u32(),cp=r.u32();connection=r.u64();stop_name=r.str();r.end();
            if(pid!=backend_pid||bits!=32||cp!=GetACP()||!connection)throw Rejected("后台握手参数无效");
            stop.reset(OpenEventA(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,stop_name.c_str()));if(!stop)win_error("打开停止输入事件");
            try{LOG_INFO("连接独立后台 pid=%lu protocol=%u stop_event=%s",backend_pid,version,stop_name.c_str());}catch(...){}
            return true;
        }catch(...){disconnect();throw;}
    }
    Packet call(Command command,std::uint64_t id=0,Buffer body={},bool input=false)
    {
        if(input)input_ready();if(!connect(false))throw Rejected("没有可连接的后台进程");
        Packet request;request.command=command;request.epoch=epoch;request.id=id;request.body.u64(rejected.load());request.body.u64(acknowledged);
        request.body.data.insert(request.body.data.end(),body.data.begin(),body.data.end());
        try{
            control.send(request,input?stop.value:nullptr,process.value);
            auto response=control.receive(control_limit,input?stop.value:nullptr,process.value);
            result(response,command,id);return response;
        }catch(const Rejected &){throw;}
        catch(...){disconnect();throw;}
    }
    void open_data()
    {
        if(data)return;data=open_pipe(base+L".data",false,false);ULONG pid=0;
        if(!GetNamedPipeServerProcessId(data.handle(),&pid)||pid!=backend_pid)throw Rejected("数据管道后台身份错误");
        Packet hello;hello.command=Command::Hello;hello.epoch=epoch;hello.body.u64(connection);
        data.send(hello,stop.value,process.value);const auto response=data.receive(control_limit,stop.value,process.value);
        if(result(response,Command::Hello,0)!=Result::Ok)throw Rejected("数据管道握手失败");
    }
    WriterStatus query()
    {
        auto response=call(Command::Query);Reader r(response.body.data);r.u32();cached=decode_status(r);r.end();
        cached.waiting+=waiting.load();return cached;
    }
};
ProcessWriter::ProcessWriter(const std::string &directory,HANDLE job,std::uint64_t lifetime):impl_(new Impl(directory,job,lifetime)){}
ProcessWriter::~ProcessWriter()=default; // DLL 卸载通过独占作业句柄结束后台，此处不等待管道或排空。
void ProcessWriter::start(AsyncLimits limits,const std::string &csv_dir,const std::string &worker_path)
{
    auto &p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);
    const auto target=p.configured_executable(worker_path);
    if(!limits.capacity||!limits.bytes||!limits.workers||limits.workers>kMaxWorkers)throw Rejected("后台队列配置无效");
    if(p.active&&(limits.capacity!=p.limits.capacity||limits.bytes!=p.limits.bytes||limits.workers!=p.limits.workers))throw Rejected("后台额度已变化，请先排空");
    if(!p.active&&p.reported){
        if(p.process&&WaitForSingleObject(p.process.value,0)==WAIT_TIMEOUT)throw Rejected("原后台尚未停机，请重新排空");
        p.disconnect();p.process.reset();p.stop.reset();p.epoch=p.connection=p.acknowledged=0;p.backend_pid=0;p.rejected=0;p.fault=false;p.cached={};
    }
    if(p.fault)throw Rejected("后台发生故障，请先执行排空确认失败");
    if(p.process&&WaitForSingleObject(p.process.value,0)==WAIT_TIMEOUT&&!p.executable.empty()&&p.executable!=target)
        throw Rejected("后台程序路径已变化，请先排空停机");
    p.executable=target;
    p.connect(true);Buffer b;b.u32(static_cast<std::uint32_t>(limits.capacity));b.u64(limits.bytes);b.u32(static_cast<std::uint32_t>(limits.workers));b.str(csv_dir);
    auto response=p.call(Command::Start,0,std::move(b),true);
    if(p.result(response,Command::Start,0)!=Result::Ok)throw std::bad_alloc();
    p.limits=limits;p.active=true;p.reported=false;
}
std::uint64_t ProcessWriter::reserve(std::uint64_t bytes)
{
    auto &p=*impl_;auto next_report=GetTickCount64();bool waited=false;std::uint64_t request=0;unsigned reconnects=0;
    {std::lock_guard<std::mutex> lock(p.mutex);if(p.request_serial==UINT64_MAX)throw Rejected("管道请求编号耗尽");request=++p.request_serial;}
    for(;;){
        try{std::lock_guard<std::mutex> lock(p.mutex);Buffer b;b.u64(bytes);auto response=p.call(Command::Reserve,request,std::move(b),true);Reader r(response.body.data);
            const auto status=static_cast<Result>(r.u32());
            if(status==Result::Ok){const auto id=r.u64();r.end();if(waited)try{LOG_INFO("独立后台资源恢复 id=%llu",id);}catch(...){};return id;}
            if(status!=Result::Wait)throw Rejected("后台预留响应无效");
        }catch(const Rejected &){throw;}
        catch(const std::bad_alloc &){std::lock_guard<std::mutex> lock(p.mutex);p.disconnect();}
        catch(const std::exception &){std::lock_guard<std::mutex> lock(p.mutex);p.disconnect();p.input_ready();if(++reconnects>3){p.fault=true;throw Rejected("预留结果无法核实，请先排空");}}
        waited=true;if(GetTickCount64()>=next_report){next_report=GetTickCount64()+5000;try{LOG_WARN("%s","等待独立后台资源 retry_ms=200，队列或系统提交额度不足");}catch(...){};}
        wait_allocation();
    }
}
void ProcessWriter::check_memory(std::uint64_t additional)
{
    auto next_report=GetTickCount64();
    for(;;){MEMORYSTATUSEX info{};info.dwLength=sizeof(info);if(!GlobalMemoryStatusEx(&info))win_error("读取前台提交额度");
        if(additional<=info.ullAvailPageFile)return;
        if(GetTickCount64()>=next_report){next_report=GetTickCount64()+5000;try{LOG_WARN("前台暂存等待 commit_available=%llu additional=%llu retry_ms=200",info.ullAvailPageFile,additional);}catch(...){};}
        wait_allocation();
    }
}
void ProcessWriter::wait_allocation()
{
    auto &p=*impl_;++p.waiting;try{p.pause();}catch(...){--p.waiting;throw;}--p.waiting;
}
void ProcessWriter::release(std::uint64_t reservation) noexcept
{
    if(!reservation)return;auto &p=*impl_;
    try{std::lock_guard<std::mutex> lock(p.mutex);auto response=p.call(Command::Release,reservation);if(p.result(response,Command::Release,reservation)!=Result::Ok)throw std::bad_alloc();}catch(...){p.fault=true;}
}
void ProcessWriter::submit(std::uint64_t id,ExportJob &&job)
{
    const auto begin=ExportClock::now();double transfer_ms=0,confirm_ms=0;std::uint64_t payload_bytes=0;
    auto &p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);Buffer meta;encode_job(meta,job);
    auto checksum=crc32(0,meta.data.data(),meta.data.size());
    for(unsigned field=0;field<8;++field){auto &values=array(job,field);payload_bytes+=values.size()*sizeof(float);checksum=crc32(checksum,values.data(),values.size()*sizeof(float));}
    const auto crc_ms=std::chrono::duration<double,std::milli>(ExportClock::now()-begin).count();
    unsigned reconnects=0;
    for(;;){
        try{
            Buffer identity;identity.u32(checksum);auto status=p.call(Command::TaskStatus,id,std::move(identity),true);const auto stage=p.result(status,Command::TaskStatus,id);
            if(stage!=Result::Accepted&&stage!=Result::Ready){
                const auto transfer_begin=ExportClock::now();
                p.open_data();Packet begin;begin.command=Command::Begin;begin.epoch=p.epoch;begin.id=id;begin.body=meta;
                p.data.send(begin,p.stop.value,p.process.value);auto ready=p.data.receive(control_limit,p.stop.value,p.process.value,0);
                if(p.result(ready,Command::Begin,id)!=Result::Ready)throw Rejected("后台尚未准备接收数据");
                for(unsigned field=0;field<8;++field){auto &values=array(job,field);std::size_t offset=0;
                    while(offset<values.size()){
                        p.input_ready();const auto count=std::min(values.size()-offset,(data_limit-8)/sizeof(float));
                        Packet packet;packet.command=Command::Data;packet.epoch=p.epoch;packet.id=id;packet.body.u32(field);packet.body.u32(static_cast<std::uint32_t>(offset));
                        const auto bytes=reinterpret_cast<const unsigned char *>(values.data()+offset);packet.body.data.insert(packet.body.data.end(),bytes,bytes+count*sizeof(float));
                        p.data.send(packet,p.stop.value,p.process.value,0);offset+=count;
                    }
                }
                Packet end;end.command=Command::DataEnd;end.epoch=p.epoch;end.id=id;end.body.u32(checksum);
                end.body.real(job.started==ExportClock::time_point{}?0:std::chrono::duration<double,std::milli>(ExportClock::now()-job.started).count());
                p.data.send(end,p.stop.value,p.process.value);
                ready=p.data.receive(control_limit,p.stop.value,p.process.value,0);if(p.result(ready,Command::DataEnd,id)!=Result::Ready)throw Rejected("后台未完整接收任务");
                transfer_ms+=std::chrono::duration<double,std::milli>(ExportClock::now()-transfer_begin).count();
            }
            const auto confirm_begin=ExportClock::now();
            Buffer commit;commit.u32(checksum);auto accepted=p.call(Command::Commit,id,std::move(commit),true);
            confirm_ms+=std::chrono::duration<double,std::milli>(ExportClock::now()-confirm_begin).count();
            if(p.result(accepted,Command::Commit,id)==Result::Wait){p.pause();continue;}
            if(p.result(accepted,Command::Commit,id)!=Result::Accepted)throw Rejected("后台没有确认入队");
            // 已收到入队确认后才释放源数组；清理回执失败不能撤销已经接收的任务。
            p.acknowledged=id;
            try{LOG_INFO("管道交接完成 backend_pid=%lu id=%llu payload_bytes=%llu crc_ms=%.3f transfer_ms=%.3f confirm_ms=%.3f handoff_ms=%.3f reconnects=%u",p.backend_pid,id,payload_bytes,crc_ms,transfer_ms,confirm_ms,std::chrono::duration<double,std::milli>(ExportClock::now()-begin).count(),reconnects);}catch(...){}
            job=ExportJob{};
            try{auto receipt=p.call(Command::Receipt,id);if(p.result(receipt,Command::Receipt,id)!=Result::Ok)p.disconnect();}catch(...){p.disconnect();}
            return;
        }catch(const Rejected &){throw;}
        catch(const std::bad_alloc &){p.disconnect();p.pause();}
        catch(const std::exception &){p.disconnect();p.input_ready();if(++reconnects>3){p.fault=true;throw Rejected("任务交接结果无法核实，请排空并检查后台日志");}p.pause();}
    }
}
void ProcessWriter::note_rejected() noexcept {++impl_->rejected;impl_->reported=false;}
WriterStatus ProcessWriter::status()
{
    auto &p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);
    if(p.reported)return p.cached;
    if(!p.control&&!p.connect(false)){auto s=p.cached;s.rejected=p.rejected.load();s.waiting=p.waiting.load();return s;}
    try{return p.query();}catch(...){p.fault=true;throw;}
}
bool ProcessWriter::finish()
{
    auto &p=*impl_;std::unique_lock<std::mutex> lock(p.mutex);
    try{
        if(!p.control&&!p.connect(false)){p.cached.rejected=p.rejected.load();p.reported=true;return !p.fault&&!p.cached.rejected;}
        p.call(Command::Drain);
        for(;;){
            auto response=p.call(Command::Query);Reader r(response.body.data);const auto state=static_cast<Result>(r.u32());p.cached=decode_status(r);r.end();
            if(state==Result::Done)break;
            lock.unlock();Sleep(200);lock.lock();
        }
        try{p.call(Command::FinalAck);}catch(...){if(WaitForSingleObject(p.process.value,1000)==WAIT_TIMEOUT)p.call(Command::FinalAck);}
        if(WaitForSingleObject(p.process.value,10000)!=WAIT_OBJECT_0)throw Rejected("后台未确认退出");
        DWORD code=999;if(!GetExitCodeProcess(p.process.value,&code)||code!=0)throw Rejected("后台异常退出");
        p.disconnect();p.active=false;p.reported=true;
        return !p.fault&&!p.cached.failed&&!p.cached.rejected&&!p.cached.unfinished;
    }catch(const std::exception &error){p.fault=true;p.reported=true;p.active=false;p.cached.rejected=std::max(p.cached.rejected,p.rejected.load()+1);try{LOG_ERROR("独立后台排空失败：%s",error.what());}catch(...){};return false;}
}
void ProcessWriter::cancel_input() noexcept
{
    auto &p=*impl_;if(p.stop)SetEvent(p.stop.value);
    try{std::lock_guard<std::mutex> lock(p.mutex);if(p.connect(false))p.call(Command::Cancel);else ++p.rejected;}catch(...){p.fault=true;}
}
std::string ProcessWriter::cancel_event_name(){std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->stop_name;}
}
