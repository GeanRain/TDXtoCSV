// 用隔离CSV副本测量真实保存核心与线程池，不改配置、不读写通达信目录。
// 参数：样本目录（000001.SZ.1m/5m.csv）、新输出目录、保存线程数、任务数。
#include "../shared/csv_store.cpp"
#include "../SaveAsCSV/export/process_writer.h"
#include <filesystem>
#include <cstring>
#include <iostream>
#include <mutex>
#include <psapi.h>
using namespace tdx_export;
namespace fs=std::filesystem;
void Logger::log(const char *,const char *,...){}
const char *kDefaultIni="[log]\nmode=disable\n";
namespace {
std::mutex measured;double phases[4]{},saved_ms=0;std::uint64_t completed=0;
bool save(ExportJob &job){const auto ok=perform_save(job);std::lock_guard<std::mutex> lock(measured);for(int i=0;i<4;++i)phases[i]+=job.save_ms[i];saved_ms+=job.save_call_ms;++completed;return ok;}
std::uint64_t cpu_time(HANDLE process=GetCurrentProcess()){FILETIME a,b,k,u;if(!GetProcessTimes(process,&a,&b,&k,&u))throw std::runtime_error("读取进程CPU时间失败");return ((std::uint64_t(k.dwHighDateTime)<<32)|k.dwLowDateTime)+((std::uint64_t(u.dwHighDateTime)<<32)|u.dwLowDateTime);}
ExportJob sample(const fs::path &file,int period){
    ExportJob job;job.native_period=period;job.period=period_name(period);job.symbol="000001.SZ";job.minute=true;
    std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("样本无法打开");std::string line;std::getline(input,line);
    while(std::getline(input,line)){
        CsvRow row;if(!parse_csv_row(line,9,row,job))throw std::runtime_error("样本不是有效CSV");
        const auto &ts=row.ts;job.dates.push_back(static_cast<float>(row.date-19000000));job.times.push_back(static_cast<float>(std::stoi(ts.substr(11,2))*100+std::stoi(ts.substr(14,2))));
        const int order[]={0,3,1,2,4,5};for(int i=0;i<6;++i)job.series[i].push_back(row.values[order[i]]);
    }
    if(!input.eof()||input.bad()||job.dates.empty())throw std::runtime_error("样本读取不完整");job.count=static_cast<int>(job.dates.size());return job;
}
}
int main(int argc,char **argv){
    SetConsoleOutputCP(CP_UTF8);try{
        if(argc!=5&&argc!=6)return 2;const bool pipe=argc==6;const auto source=fs::absolute(argv[1]),root=fs::absolute(argv[2]);const int workers=std::stoi(argv[3]),tasks=std::stoi(argv[4]);
        if(fs::exists(root)||workers<1||workers>128||tasks<1||tasks>128)throw std::runtime_error("需要全新输出目录及有效并行数");
        ExportJob samples[]={sample(source/"000001.SZ.1m.csv",0),sample(source/"000001.SZ.5m.csv",1)};
        std::vector<ExportJob> jobs;std::uint64_t rows=0;
        for(int i=0;i<tasks;++i){auto j=samples[i%2];const auto dir=root/std::to_string(i);fs::create_directories(dir);j.csv_dir=dir.string();fs::copy_file(source/(j.symbol+"."+j.period+".csv"),dir/(j.symbol+"."+j.period+".csv"));rows+=j.count;jobs.push_back(std::move(j));}
        AsyncWriter writer(save);ProcessWriter remote(root.string());AsyncLimits limits;limits.capacity=tasks+1;limits.bytes=1024ULL*1024*1024;limits.workers=workers;HANDLE backend=nullptr;
        if(pipe){
            fs::copy_file(fs::absolute(argv[5]),root/"SaveAsCSVWorker.exe");std::ofstream ini(root/"saveAsCVS.ini",std::ios::binary);ini<<"[log]\nmode=file\nlevel=1\nfile=bench.log\n[async]\nqueue_capacity="<<limits.capacity<<"\nqueue_memory_mb=1024\nworker_threads="<<workers<<'\n';ini.close();remote.start(limits,(root/"status").string());
            for(const auto &file:fs::directory_iterator(root)){const auto name=file.path().filename().string();const auto at=name.find("_worker_");if(at!=std::string::npos){const auto pid=std::stoul(name.substr(at+8));backend=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);break;}}
            if(!backend)throw std::runtime_error("无法读取基准后台统计");
        }else writer.start(limits);
        const auto cpu=cpu_time()+(backend?cpu_time(backend):0);const auto begin=ExportClock::now();
        for(auto &j:jobs){const auto id=pipe?remote.reserve(job_memory(j)+1024):writer.reserve(job_memory(j)+1024);if(pipe)remote.submit(id,std::move(j));else writer.submit(id,std::move(j));}
        const auto submitted=ExportClock::now();if(!(pipe?remote.finish():writer.finish()))throw std::runtime_error("基准保存失败");const auto wall=elapsed_ms(begin);const auto cpu_ms=(cpu_time()+(backend?cpu_time(backend):0)-cpu)/10000.0;
        PROCESS_MEMORY_COUNTERS_EX memory{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory));
        if(backend)CloseHandle(backend);
        if(pipe){
            for(const auto &file:fs::directory_iterator(root))if(file.path().filename().string().find("_worker_")!=std::string::npos){
                std::ifstream log(file.path(),std::ios::binary);std::string line;while(std::getline(log,line)){
                    auto value=[&](const char *key){const auto at=line.find(std::string(key)+"=");if(at==std::string::npos)throw std::runtime_error("缺少后台性能字段");return std::stod(line.substr(at+std::strlen(key)+1));};
                    if(line.find("导出耗时")!=std::string::npos){saved_ms+=value("save_ms");++completed;}
                    if(line.find("保存分段")!=std::string::npos){const char *keys[]={"prepare_ms","read_ms","merge_ms","write_ms"};for(int i=0;i<4;++i)phases[i]+=value(keys[i]);}
                }
            }
            if(completed!=static_cast<std::uint64_t>(tasks))throw std::runtime_error("后台性能报告不完整");
        }
        std::cout<<"mode="<<(pipe?"pipe":"core")<<" workers="<<workers<<" tasks="<<completed<<" rows="<<rows<<" front_ms="<<std::chrono::duration<double,std::milli>(submitted-begin).count()<<" wall_ms="<<wall<<" cpu_ms="<<cpu_ms<<" cpu_cores="<<cpu_ms/wall<<" save_sum_ms="<<saved_ms<<" prepare_ms="<<phases[0]<<" read_ms="<<phases[1]<<" merge_ms="<<phases[2]<<" write_ms="<<phases[3]<<" peak_private_bytes="<<memory.PeakPagefileUsage<<'\n';return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
