// 真实DLL交接后退出宿主：后台必须立即结束，不等待被屏障暂停的保存任务。
#include "../shared/ipc.h"
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
using tdx_export::ipc::Handle;
using Fn=void (*)(int,float *,float *,float *,float *);
void check(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void write(const fs::path &file,const std::string &text){fs::create_directories(file.parent_path());std::ofstream out(file,std::ios::binary);out<<text;check(out.good(),"写测试文件失败");}
std::string read(const fs::path &file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
int child(const fs::path &dll){
    const auto module=LoadLibraryW(dll.c_str());check(module!=nullptr,"无法加载被测DLL");
    auto fn=[&](int id){auto f=reinterpret_cast<Fn>(GetProcAddress(module,("Func"+std::to_string(id)).c_str()));check(f!=nullptr,"缺少导出函数");return f;};
    float out[2]{},dates[]={1261019,1261020},data[2]{},market=1,period=5,zero=0,one=1,command=3;
    auto call=[&](int id,float *a,float *b,float *c){fn(id)(2,out,a,b,c);check(out[0]==1&&out[1]==1,"宿主接口调用失败");};
    for(float code:{600031.f,600032.f}){
        call(4,&code,&market,&period);call(5,&one,&zero,&zero);call(2,dates,nullptr,&one);
        const float values[]={10,11,12,9,100,1000};for(int i=0;i<6;++i){data[0]=data[1]=values[i];float type=static_cast<float>(i+1);call(3,&type,data,&one);}call(1,&command,&one,&zero);
    }
    write(dll.parent_path()/"accepted.txt","accepted");char name[128]{};
    check(GetEnvironmentVariableA("TDX_TEST_HOST_EXIT",name,sizeof(name))>0,"缺少退出事件");Handle exit(OpenEventA(SYNCHRONIZE,FALSE,name));check(bool(exit),"退出事件不存在");WaitForSingleObject(exit.value,INFINITE);
    return 0;
}
void scenario(const fs::path &dllSource,const fs::path &workerSource,const fs::path &root,bool crash,bool replacing){
    const auto dir=root/(std::string(crash?"crash-":"normal-")+(replacing?"replace":"queue"));fs::create_directories(dir);
    const auto dll=dir/"SaveAsCSV.dll";fs::copy_file(dllSource,dll);fs::copy_file(workerSource,dir/"SaveAsCSVWorker.exe");
    write(dir/"saveAsCVS.ini","[log]\nmode=file\nlevel=1\nfile=exit.log\n[export]\ncsv_dir=csv\n[async]\nworker_threads=1\n");
    const auto csv=dir/"csv"/"600031.SH.1d.csv";
    const std::string original="symbol,period,ts,open,high,low,close,volume,amount\n600031.SH,1d,2026-10-16 15:00:00,10,12,9,11,100,1000\n";write(csv,original);
    const auto prefix="Local\\SaveAsCSV_ExitTest_"+std::to_string(GetCurrentProcessId())+"_"+std::to_string(GetTickCount64());
    const auto gate_name=prefix+"_gate",ready_name=prefix+"_ready",exit_name=prefix+"_exit";
    Handle gate(CreateEventA(nullptr,TRUE,FALSE,gate_name.c_str())),ready(CreateEventA(nullptr,TRUE,FALSE,ready_name.c_str())),exit(CreateEventA(nullptr,TRUE,FALSE,exit_name.c_str()));check(bool(gate)&&bool(ready)&&bool(exit),"创建测试事件失败");
    SetEnvironmentVariableA("TDX_TEST_GATE",replacing?nullptr:gate_name.c_str());SetEnvironmentVariableA("TDX_TEST_REPLACE_GATE",replacing?gate_name.c_str():nullptr);SetEnvironmentVariableA("TDX_TEST_REPLACE_READY",replacing?ready_name.c_str():nullptr);SetEnvironmentVariableA("TDX_TEST_HOST_EXIT",exit_name.c_str());
    wchar_t self[MAX_PATH]{};check(GetModuleFileNameW(nullptr,self,MAX_PATH)>0,"查找测试宿主失败");auto args=L"\""+std::wstring(self)+L"\" --child \""+dll.wstring()+L"\"";
    std::vector<wchar_t> command(args.begin(),args.end());command.push_back(0);STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    check(CreateProcessW(self,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,dir.c_str(),&si,&pi)!=FALSE,"创建宿主失败");Handle host(pi.hProcess),thread(pi.hThread),backend;
    for(const char *name:{"TDX_TEST_GATE","TDX_TEST_REPLACE_GATE","TDX_TEST_REPLACE_READY","TDX_TEST_HOST_EXIT"})SetEnvironmentVariableA(name,nullptr);
    try{
        const auto deadline=GetTickCount64()+15000;while(!fs::exists(dir/"accepted.txt")&&GetTickCount64()<deadline&&WaitForSingleObject(host.value,0)==WAIT_TIMEOUT)Sleep(10);
        check(fs::exists(dir/"accepted.txt"),"宿主没有完成两项交接");
        for(const auto &item:fs::directory_iterator(dir)){if(item.path().extension()!=".log")continue;const auto text=read(item.path());const auto at=text.find("独立后台启动 pid=");if(at!=std::string::npos){const auto pid=std::stoul(text.substr(at+std::string("独立后台启动 pid=").size()));backend.reset(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,pid));break;}}
        check(bool(backend),"无法确认本用例后台");
        if(replacing)check(WaitForSingleObject(ready.value,10000)==WAIT_OBJECT_0,"保存没有到达替换前屏障");
        check(WaitForSingleObject(backend.value,0)==WAIT_TIMEOUT,"宿主仍活着时后台不应退出");
        if(crash)TerminateProcess(host.value,0);else SetEvent(exit.value);
        check(WaitForSingleObject(host.value,10000)==WAIT_OBJECT_0,"宿主未正常结束");
        check(WaitForSingleObject(backend.value,5000)==WAIT_OBJECT_0,"宿主已退出，后台仍等待未完成保存");
        DWORD code=0;check(GetExitCodeProcess(backend.value,&code)&&code==ERROR_PROCESS_ABORTED,"随宿主终止不得伪报正常排空");
        check(read(csv)==original,"中断保存破坏了旧CSV");check(!fs::exists(dir/"csv"/"600032.SH.1d.csv"),"宿主退出后仍继续保存排队任务");
        if(replacing){bool temporary=false;for(const auto &file:fs::directory_iterator(dir/"csv"))if(file.path().extension()==".tmp")temporary=true;check(temporary,"替换前测试未生成完整临时文件");}
    }catch(...){TerminateProcess(host.value,3);if(backend){TerminateProcess(backend.value,3);WaitForSingleObject(backend.value,5000);}throw;}
}
int main(int argc,char **argv){
    SetConsoleOutputCP(CP_UTF8);try{
        if(argc==3&&std::string(argv[1])=="--child")return child(argv[2]);
        check(argc==4,"需要DLL、测试Worker和隔离目录");int failed=0;
        for(bool crash:{false,true})for(bool replacing:{false,true})try{scenario(fs::absolute(argv[1]),fs::absolute(argv[2]),fs::absolute(argv[3]),crash,replacing);std::cout<<"通过 "<<(crash?"异常":"正常")<<"宿主退出立即结束后台，"<<(replacing?"替换前":"队列积压")<<"旧CSV保持\n";}catch(const std::exception &e){++failed;std::cout<<"失败："<<e.what()<<'\n';}
        return failed?1:0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 3;}
}