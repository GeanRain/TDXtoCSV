// 独立后台集成：真实进程和文件，屏障/分配/回包故障只存在于测试Worker。
#include "../SaveAsCSV/export/process_writer.h"
#include "../shared/ipc.h"
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <functional>
#include <map>
#include <thread>
#include <psapi.h>
namespace fs=std::filesystem;
using namespace tdx_export;
using namespace tdx_export::ipc;
const char *kDefaultIni="[log]\nmode=disable\n";
void check(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void wait_until(const std::function<bool()> &ready,const char *why){const auto end=GetTickCount64()+20000;while(!ready()){check(GetTickCount64()<end,why);Sleep(10);}}
std::string read(const fs::path &file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
void write(const fs::path &file,const std::string &text){fs::create_directories(file.parent_path());std::ofstream out(file,std::ios::binary);out<<text;check(out.good(),"写隔离文件失败");}
const char *variables[]={"TDX_TEST_GATE","TDX_TEST_ALLOC_FAILURES","TDX_TEST_DROP_RESERVE","TDX_TEST_DROP_COMMIT","TDX_TEST_DROP_FINAL","TDX_TEST_STARTUP_EXIT"};
struct Fixture {
    fs::path dir;AsyncLimits limits;std::unique_ptr<ProcessWriter> writer;Handle gate;bool finished=false;
    std::vector<std::thread> threads;
    Fixture(const fs::path &source,const fs::path &directory,int count=4,int mb=128,int workers=2,const std::string &relative="SaveAsCSVWorker.exe",const std::map<std::string,std::string> &options={},bool gated=false,bool copy=true):dir(directory){
        fs::create_directories(dir);for(auto name:variables)SetEnvironmentVariableA(name,nullptr);
        for(const auto &pair:options)SetEnvironmentVariableA(pair.first.c_str(),pair.second.c_str());
        if(gated){const auto name="Local\\SaveAsCSV_TestGate_"+std::to_string(GetCurrentProcessId())+"_"+std::to_string(GetTickCount64());gate.reset(CreateEventA(nullptr,TRUE,FALSE,name.c_str()));check(bool(gate),"创建测试屏障失败");SetEnvironmentVariableA("TDX_TEST_GATE",name.c_str());}
        if(copy){const auto target=fs::path(relative_executable(dir.wstring(),relative));fs::create_directories(target.parent_path());fs::copy_file(source,target,fs::copy_options::overwrite_existing);}
        write(dir/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=worker.log\n[async]\nqueue_capacity="+std::to_string(count)+"\nqueue_memory_mb="+std::to_string(mb)+"\nworker_threads="+std::to_string(workers)+"\nworker_path="+relative+"\n");
        limits.capacity=count;limits.bytes=static_cast<std::uint64_t>(mb)*1024*1024;limits.workers=workers;
        writer=std::make_unique<ProcessWriter>(narrow(dir.wstring()));
    }
    ~Fixture(){
        if(gate)SetEvent(gate.value);
        if(!finished&&writer)writer->cancel_input();
        for(auto &thread:threads)if(thread.joinable())thread.join();
        if(!finished&&writer)writer->finish();
        for(auto name:variables)SetEnvironmentVariableA(name,nullptr);
    }
    void start(){writer->start(limits,narrow((dir/L"csv").wstring()));}
    bool finish(){const auto ok=writer->finish();finished=true;return ok;}
    std::future<std::uint64_t> spawn(std::function<std::uint64_t()> action){
        auto promise=std::make_shared<std::promise<std::uint64_t>>();auto result=promise->get_future();
        threads.emplace_back([promise,action]{try{promise->set_value(action());}catch(...){promise->set_exception(std::current_exception());}});return result;
    }
    std::string logs(){std::string result;for(const auto &item:fs::directory_iterator(dir))if(item.path().extension()==".log")result+=read(item.path());return result;}
    DWORD pid(){const auto text=logs();const std::string marker="独立后台启动 pid=";const auto at=text.find(marker);check(at!=std::string::npos,"缺少后台进程号");return static_cast<DWORD>(std::stoul(text.substr(at+marker.size())));}
};
ExportJob job(Fixture &f,int count=2,const std::string &symbol="600000.SH"){
    ExportJob j;j.count=count;j.symbol=symbol;j.period="1d";j.native_period=5;j.csv_dir=narrow((f.dir/L"csv").wstring());
    int year=2020,month=1,day=1;for(int i=0;i<count;++i){j.dates.push_back(static_cast<float>(year*10000+month*100+day-19000000));const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};const bool leap=year%400==0||(year%4==0&&year%100!=0);if(++day>days[month-1]+(month==2&&leap)){day=1;if(++month>12){month=1;++year;}}}
    const float values[]={10,11,12,9,100,1000};for(int i=0;i<6;++i)j.series[i].assign(count,values[i]);return j;
}
void submit(Fixture &f,ExportJob j){const auto id=f.writer->reserve(job_memory(j)+1024);f.writer->submit(id,std::move(j));check(j.dates.empty(),"入队确认后没有释放源数组");}
// 独立协议客户端，直接构造残包和非法消息，不能依赖生产发送端始终发对。
struct RawClient {
    Channel control,data;std::uint64_t epoch=0,connection=0;
    explicit RawClient(Fixture &f){
        f.writer.reset();const auto base=pipe_base(GetCurrentProcessId(),creation_time(GetCurrentProcess()),f.dir.wstring());
        auto open=[&](const wchar_t *suffix){Channel channel;wait_until([&]{Handle h(CreateFileW((base+suffix).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));if(h)channel=Channel(std::exchange(h.value,nullptr));return bool(channel);},"协议测试连接失败");return channel;};
        control=open(L".control");Packet hello;hello.body.u32(GetCurrentProcessId());hello.body.u64(creation_time(GetCurrentProcess()));hello.body.u32(GetACP());hello.body.u64(0);hello.body.u64(0);
        control.send(hello);auto response=control.receive();epoch=response.epoch;Reader r(response.body.data);check(r.u32()==static_cast<unsigned>(Result::Ok),"控制握手失败");r.u32();r.u32();r.u32();connection=r.u64();r.str();r.end();
        data=open(L".data");hello={};hello.epoch=epoch;hello.body.u64(connection);data.send(hello);response=data.receive();Reader dr(response.body.data);check(dr.u32()==static_cast<unsigned>(Result::Ok),"数据握手失败");
    }
    Packet call(Command command,std::uint64_t id=0,Buffer extra={}){
        Packet p;p.command=command;p.epoch=epoch;p.id=id;p.body.u64(0);p.body.u64(0);p.body.data.insert(p.body.data.end(),extra.data.begin(),extra.data.end());control.send(p);return control.receive();
    }
    Result state(const Packet &p){Reader r(p.body.data);return static_cast<Result>(r.u32());}
    WriterStatus status(){auto p=call(Command::Query);Reader r(p.body.data);r.u32();return decode_status(r);}
    std::uint64_t begin(const ExportJob &j){
        Buffer size;size.u64(job_memory(j)+1024);auto response=call(Command::Reserve,1,std::move(size));Reader r(response.body.data);check(r.u32()==static_cast<unsigned>(Result::Ok),"协议测试预留失败");auto id=r.u64();
        Packet p;p.command=Command::Begin;p.id=id;p.epoch=epoch;encode_job(p.body,j);data.send(p);check(state(data.receive())==Result::Ready,"未准备接收");return id;
    }
    std::uint32_t arrays(std::uint64_t id,ExportJob &j){
        Buffer meta;encode_job(meta,j);auto crc=crc32(0,meta.data.data(),meta.data.size());
        for(unsigned field=0;field<8;++field){auto &v=array(j,field);if(v.empty())continue;Packet p;p.command=Command::Data;p.epoch=epoch;p.id=id;p.body.u32(field);p.body.u32(0);auto bytes=reinterpret_cast<const unsigned char *>(v.data());p.body.data.insert(p.body.data.end(),bytes,bytes+v.size()*4);data.send(p);crc=crc32(crc,bytes,v.size()*4);}return crc;
    }
    void end(std::uint64_t id,std::uint32_t crc){Packet p;p.command=Command::DataEnd;p.epoch=epoch;p.id=id;p.body.u32(crc);p.body.real(0);data.send(p);check(state(data.receive())==Result::Ready,"完整数据未准备提交");}
    void close(){data.close();control.close();}
};
std::uint64_t private_bytes(HANDLE process){PROCESS_MEMORY_COUNTERS_EX memory{};check(GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory))!=FALSE,"读取隔离进程内存失败");return memory.PrivateUsage;}
int main(int argc,char **argv){
    // 把本测试程序作为假后台，模拟在创建管道和初始化日志前退出。
    if(argc==5&&std::string(argv[1])=="--parent"){
        char code[32]{};GetEnvironmentVariableA("TDX_TEST_STARTUP_EXIT",code,sizeof(code));
        ExitProcess(static_cast<DWORD>(std::strtoul(code,nullptr,0)));
    }
    SetConsoleOutputCP(CP_UTF8);if(argc!=4)return 2;
    const auto production=fs::absolute(argv[1]),injected=fs::absolute(argv[2]),root=fs::absolute(argv[3]);fs::create_directories(root);
    int passed=0,failed=0;auto run=[&](const char *name,const std::function<void()> &action){try{action();++passed;std::cout<<"通过 "<<name<<'\n';}catch(const std::exception &e){++failed;std::cout<<"失败 "<<name<<"："<<e.what()<<'\n';}};
    for(const std::string code:{"0x00000002","0xC0000135"})run(("启动阶段退出立即报告原退出码 "+code).c_str(),[&]{
        Fixture f(fs::absolute(argv[0]),root/("startup-"+code),4,128,2,"SaveAsCSVWorker.exe",{{"TDX_TEST_STARTUP_EXIT",code}});
        const auto begin=GetTickCount64();std::string error;try{f.start();}catch(const std::exception &e){error=e.what();}
        const auto waited=GetTickCount64()-begin;std::cout<<"启动退出样本 exit_code="<<code<<" waited_ms="<<waited<<'\n';
        check(error.find(code)!=std::string::npos,"启动失败没有返回原退出码");check(waited<3000,"后台已退出仍等待启动超时");
        check(!fs::exists(f.dir/"csv"),"假后台不应创建CSV目录");check(!f.finish(),"后台启动失败不得报告排空成功");
    });
    for(const std::string relative:{"SaveAsCSVWorker.exe","backend\\SaveAsCSVWorker.exe","..\\bin\\SaveAsCSVWorker.exe"})run(("真实后台相对路径与保存 "+relative).c_str(),[&]{
        Fixture f(production,root/("path-"+std::to_string(passed+failed))/"dlls",4,128,2,relative);f.start();submit(f,job(f));check(f.finish(),"排空失败");
        const auto s=f.writer->status();check(s.accepted==1&&s.succeeded==1&&!s.failed&&!s.bytes&&!s.unfinished,"最终统计错误");
        const auto text=read(f.dir/"csv"/"600000.SH.1d.csv");check(std::count(text.begin(),text.end(),'\n')==3,"CSV不完整");
        check(fs::exists(f.dir/"csv"/"_bg_0-4_0-2"),"状态未归零");
    });
    run("真实后台重复任务跳过写盘仍释放额度并报告成功",[&]{
        Fixture f(production,root/"unchanged");f.start();submit(f,job(f));wait_until([&]{return f.writer->status().unfinished==0;},"首次保存未完成");
        const auto file=f.dir/"csv"/"600000.SH.1d.csv";const auto original=read(file);const auto modified=fs::last_write_time(file);
        Handle hold(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(bool(hold),"无法锁定原文件禁止替换");
        submit(f,job(f));submit(f,job(f));check(f.finish(),"重复任务应成功排空");const auto s=f.writer->status();
        check(s.accepted==3&&s.succeeded==3&&!s.failed&&!s.bytes&&!s.unfinished,"跳过写盘仍须计为成功并释放全部额度");
        check(read(file)==original&&fs::last_write_time(file)==modified,"重复导出不得修改CSV");check(f.logs().find("数据未变化，跳过写盘：")!=std::string::npos,"真实后台缺少跳过写盘日志");
    });
    for(const std::string variable:{"TDX_TEST_DROP_RESERVE","TDX_TEST_DROP_COMMIT","TDX_TEST_DROP_FINAL"})run(("已执行后丢回包可恢复 "+variable).c_str(),[&]{
        Fixture f(injected,root/variable,4,128,2,"SaveAsCSVWorker.exe",{{variable,"1"}});f.start();submit(f,job(f));check(f.finish(),"丢回包后排空失败");
        const auto s=f.writer->status();check(s.accepted==1&&s.succeeded==1&&!s.reserved&&!s.bytes,"丢回包造成重复接收或额度泄漏");
    });
    run("后台真实分配连续失败后仍完成且旧CSV不受损",[&]{
        Fixture f(injected,root/"alloc",4,128,2,"SaveAsCSVWorker.exe",{{"TDX_TEST_ALLOC_FAILURES","3"}});f.start();submit(f,job(f));check(f.finish(),"后台分配重试失败");check(f.logs().find("内存分配重试 phase=save")!=std::string::npos,"没有真实分配重试证据");check(f.writer->status().succeeded==1,"重试不得丢任务");
    });
    for(bool bytes:{false,true})run(bytes?"字节额度满后释放自动恢复且查询可用":"数量额度满后释放自动恢复且查询可用",[&]{
        Fixture f(injected,root/(bytes?"bytes":"count"),bytes?100:1,bytes?1:128,1,"SaveAsCSVWorker.exe",{},true);f.start();auto data=job(f,bytes?20000:2);const auto size=job_memory(data)+1024;submit(f,std::move(data));
        wait_until([&]{return f.writer->status().busy==1;},"任务没有进入保存屏障");auto future=f.spawn([&]{return f.writer->reserve(size);});
        wait_until([&]{return f.writer->status().waiting>0;},"前台没有等待额度");check(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready,"满额仍放行");
        SetEvent(f.gate.value);wait_until([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},"释放后没有自动恢复");f.writer->release(future.get());check(f.finish(),"额度恢复后排空失败");
    });
    run("外部停止事件解除等待且已接收任务继续完成",[&]{
        Fixture f(injected,root/"cancel",1,128,1,"SaveAsCSVWorker.exe",{},true);f.start();auto data=job(f);const auto size=job_memory(data)+1024;submit(f,std::move(data));
        auto future=f.spawn([&]{return f.writer->reserve(size);});wait_until([&]{return f.writer->status().waiting>0;},"没有进入资源等待");
        Handle stop(OpenEventA(EVENT_MODIFY_STATE,FALSE,f.writer->cancel_event_name().c_str()));check(bool(stop),"外部停止事件打不开");SetEvent(stop.value);
        wait_until([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},"停止不能解除等待");bool cancelled=false;try{future.get();}catch(...){cancelled=true;}check(cancelled,"取消应使当前接收失败");
        f.writer->cancel_input();SetEvent(f.gate.value);check(!f.finish(),"取消不能报告全部成功");check(f.writer->status().succeeded==1,"取消丢弃了已接收任务");
    });
    run("后台异常退出不返回假零值且排空必须失败",[&]{
        Fixture f(injected,root/"worker-crash",4,128,1,"SaveAsCSVWorker.exe",{},true);f.start();submit(f,job(f));
        Handle process(OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,f.pid()));check(bool(process),"无法打开被测后台");TerminateProcess(process.value,73);WaitForSingleObject(process.value,5000);
        bool error=false;try{f.writer->status();}catch(...){error=true;}check(error,"后台崩溃后的状态不得伪报为零");check(!f.finish(),"后台崩溃不得报告成功");
    });
    run("后台拒绝非法行情并保留旧CSV且其他股票继续保存",[&]{
        Fixture f(production,root/"protected");f.start();const auto target=f.dir/"csv"/"600000.SH.1d.csv";
        const std::string original="symbol,period,ts,open,high,low,close,volume,amount\n600000.SH,1d,2019-12-31 15:00:00,10,12,9,11,100,1000\n";write(target,original);
        auto invalid=job(f);invalid.series[4][0]=-1;submit(f,std::move(invalid));submit(f,job(f,2,"600001.SH"));check(!f.finish(),"非法行情不得报告全部成功");check(read(target)==original,"旧CSV被破坏");check(f.writer->status().failed==1&&f.writer->status().succeeded==1,"单任务失败阻止了其他文件");
    });
    run("缺失后台与绝对路径配置均明确失败",[&]{
        Fixture f(production,root/"missing",4,128,2,"missing.exe",{},false,false);bool error=false;try{f.start();}catch(...){error=true;}check(error,"缺失后台未失败");f.writer->note_rejected();check(!f.finish(),"缺失后台不得排空成功");
        ProcessWriter writer(narrow((root/"missing").wstring()));error=false;try{writer.start(f.limits,narrow((f.dir/L"csv").wstring()),"C:\\invalid.exe");}catch(...){error=true;}check(error,"绝对后台路径未拒绝");
    });
    run("运行中更改后台路径被拒绝且原任务仍可完成",[&]{
        Fixture f(production,root/"path-change");f.start();submit(f,job(f));bool error=false;try{f.writer->start(f.limits,narrow((f.dir/L"csv").wstring()),"different.exe");}catch(...){error=true;}check(error,"运行中后台路径变更未拒绝");check(f.finish(),"拒绝路径变更影响了原任务");
    });
    run("中文DLL目录与中文后台相对目录均可使用",[&]{
        Fixture f(production,root/fs::u8path("中文路径"),4,128,2,narrow(fs::u8path("后台/SaveAsCSVWorker.exe").wstring()));f.start();submit(f,job(f));check(f.finish(),"中文路径排空失败");check(fs::exists(f.dir/"csv"/"600000.SH.1d.csv"),"中文目录CSV未保存");
    });
    run("相同提交只接收一次且同号不同内容明确拒绝",[&]{
        Fixture f(production,root/"duplicate");f.start();RawClient raw(f);auto j=job(f);const auto id=raw.begin(j);const auto crc=raw.arrays(id,j);raw.end(id,crc);
        Buffer hash;hash.u32(crc);check(raw.state(raw.call(Command::Commit,id,hash))==Result::Accepted,"首次提交失败");check(raw.state(raw.call(Command::Commit,id,hash))==Result::Accepted,"重复提交未补回确认");
        hash={};hash.u32(crc^1);check(raw.state(raw.call(Command::Commit,id,hash))==Result::Rejected,"不同摘要未拒绝");check(raw.status().accepted==1,"重复入队");raw.close();
        f.writer=std::make_unique<ProcessWriter>(narrow(f.dir.wstring()));check(f.finish(),"重复提交后排空失败");check(f.writer->status().succeeded==1,"重复保存");
    });
    for(int fault=0;fault<4;++fault)run(("残片和非法数据不入队 case="+std::to_string(fault)).c_str(),[&]{
        Fixture f(production,root/("packet-"+std::to_string(fault)));f.start();RawClient raw(f);auto j=job(f);const auto id=raw.begin(j);
        if(fault==0){raw.data.close();} // 元数据已收到，数组还没有发完就断线。
        else if(fault==1){raw.arrays(id,j);raw.data.close();} // 数组完整，尚未DATA_END/COMMIT。
        else if(fault==2){const auto crc=raw.arrays(id,j);bool denied=false;try{raw.end(id,crc^1);}catch(...){denied=true;}check(denied,"错误CRC被接受");}
        else{Packet p;p.command=Command::Data;p.epoch=raw.epoch;p.id=id;p.body.u32(0);p.body.u32(1);p.body.u32(0);raw.data.send(p);bool denied=false;try{raw.data.receive();}catch(...){denied=true;}check(denied,"数组偏移乱序被接受");}
        check(raw.status().accepted==0,"残片被计为完整任务");check(!fs::exists(f.dir/"csv"/"600000.SH.1d.csv"),"残片写入CSV");raw.call(Command::Release,id);raw.close();f.writer=std::make_unique<ProcessWriter>(narrow(f.dir.wstring()));check(f.finish(),"清理残片不能干净退出");check(!f.writer->status().reserved,"残片额度泄漏");
    });
    run("真实积压驻留后台，前台只持有当前股票",[&]{
        Fixture f(injected,root/"memory-isolation",16,128,2,"SaveAsCSVWorker.exe",{},true);f.start();Handle backend(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,f.pid()));check(bool(backend),"无法读取后台内存");
        const auto front_before=private_bytes(GetCurrentProcess()),back_before=private_bytes(backend.value);
        for(int i=0;i<8;++i)submit(f,job(f,100000,"60000"+std::to_string(i)+".SH"));
        check(f.writer->status().unfinished==8,"测试没有真实积压");const auto front_after=private_bytes(GetCurrentProcess()),back_after=private_bytes(backend.value);
        const auto front_delta=front_after>front_before?front_after-front_before:0,back_delta=back_after>back_before?back_after-back_before:0;
        std::cout<<"内存隔离样本 front_delta="<<front_delta<<" backend_delta="<<back_delta<<'\n';check(back_delta>=20*1024*1024,"后台没有持有预期行情数组");check(front_delta<12*1024*1024,"前台仍保留多只股票积压");SetEvent(f.gate.value);check(f.finish(),"积压排空失败");
    });
    run("独立后台128个任务同时保存且第129个排队",[&]{
        Fixture f(injected,root/"pool-128",256,128,128,"SaveAsCSVWorker.exe",{},true);f.start();
        for(int i=0;i<129;++i)submit(f,job(f,2,std::to_string(600000+i)+".SH"));
        wait_until([&]{return f.writer->status().busy==128;},"后台没有同时运行128个保存任务");
        const auto s=f.writer->status();check(s.accepted==129&&s.unfinished==129&&s.busy==128,"超额任务必须在队列等待");
        SetEvent(f.gate.value);check(f.finish(),"128线程排空失败");check(f.writer->status().succeeded==129,"128线程丢任务");check(fs::exists(f.dir/"csv"/"_bg_0-256_0-128"),"128线程状态没有归零");
    });
    std::cout<<"进程集成："<<passed<<" 通过，"<<failed<<" 失败\n";return failed?1:0;
}
