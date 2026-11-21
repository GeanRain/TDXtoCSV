// 原生 Win32 驱动：对比相同编译参数下的真实 DLL；全部文件写入隔离目录。
#include <windows.h>
#include "../SaveAsCSV/link/tdx/FxIndicator.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Values = std::vector<float>;
using Function = void (*)(int,float *,float *,float *,float *);
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
double micros(Clock::time_point begin){return std::chrono::duration<double,std::micro>(Clock::now()-begin).count();}
std::string read(const fs::path &path){std::ifstream in(path,std::ios::binary);check(in.is_open(),"CSV未生成");return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
struct Input {
    int n;Values dates,times,series[6],raw[2],scaled[2],bars[2];
    explicit Input(int count):n(count),times(count,930){
        int year=1960,month=1,day=1;
        for(int i=0;i<n;++i){
            dates.push_back(static_cast<float>(year*10000+month*100+day-19000000));
            const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};const bool leap=year%400==0||(year%4==0&&year%100!=0);
            if(++day>days[month-1]+(month==2&&leap)){day=1;if(++month>12){month=1;++year;}}
        }
        const float values[]={10,11,12,9,100,1000};for(int field=0;field<6;++field)series[field].assign(n,values[field]);
        for(int field=0;field<2;++field){raw[field].resize(n);scaled[field].resize(n);bars[field].resize(n);for(int i=0;i<n;++i){raw[field][i]=static_cast<float>((i/(field?3:5))%8)/8.0f;bars[field][i]=static_cast<float>(i%(field?3:5));}}
        scale();
    }
    void scale(){for(int field=0;field<2;++field)for(int i=0;i<n;++i)scaled[field][i]=raw[field][i]*100.0f;}
};
struct Module {
    fs::path dir;bool paired;HMODULE library=nullptr;Function functions[9]{};Values output[12],ones;
    Module(const fs::path &dll,bool combined):dir(dll.parent_path()),paired(combined){
        std::ofstream ini(dir/"saveAsCVS.ini",std::ios::binary);ini<<"[log]\nmode=disable\n[export]\ncsv_dir=csv\n[async]\nqueue_capacity=16\nqueue_memory_mb=256\nworker_threads=1\n";ini.close();
        library=LoadLibraryW(dll.c_str());check(library!=nullptr,"加载基准DLL失败");
        for(int i=1;i<=8;++i){functions[i]=reinterpret_cast<Function>(GetProcAddress(library,("Func"+std::to_string(i)).c_str()));check(functions[i]!=nullptr,"缺少DLL接口");}
        const auto registration=reinterpret_cast<BOOL (*)(PluginTCalcFuncInfo **)>(GetProcAddress(library,"RegisterTdxFunc"));PluginTCalcFuncInfo *info=nullptr;
        check(registration&&registration(&info),"注册基准DLL失败");
    }
    ~Module(){if(library){float out=0,zero=0;functions[7](1,&out,&zero,&zero,&zero);FreeLibrary(library);}}
    void buffers(int n){for(auto &values:output)values.assign(n,-999);ones.assign(n,1);}
    void prepare(Input &input,int code,int period=5){
        if(output[0].size()!=static_cast<size_t>(input.n))buffers(input.n);
        float symbol=static_cast<float>(code),market=1,native=static_cast<float>(period),zero=0;
        functions[4](input.n,output[0].data(),&symbol,&market,&native);check(output[0][0]==1,"开始采集失败");
        functions[5](input.n,output[1].data(),output[0].data(),&zero,&zero);check(output[1][0]==1,"周期确认失败");
        functions[2](input.n,output[2].data(),input.dates.data(),input.times.data(),output[1].data());check(output[2][0]==1,"日期接收失败");
    }
    void receive(Input &input,bool events=true){
        if(paired){
            for(int type=1;type<=3;++type){float kind=static_cast<float>(type);functions[3](input.n,output[type+2].data(),&kind,input.series[(type-1)*2].data(),input.series[(type-1)*2+1].data());}
            if(events)for(int field=0;field<2;++field){float kind=static_cast<float>(4+field);functions[3](input.n,output[6+field].data(),&kind,input.bars[field].data(),input.raw[field].data());}
        }else{
            for(int type=1;type<=6;++type){float kind=static_cast<float>(type);functions[3](input.n,output[type+2].data(),&kind,input.series[type-1].data(),output[type+1].data());}
            if(events){float kind=7;functions[3](input.n,output[9].data(),&kind,input.scaled[0].data(),input.scaled[1].data());}
        }
    }
    void submit(Input &input,bool events=true){
        float command=3,zero=0;float *previous=paired?ones.data():output[events?9:8].data();
        functions[1](input.n,output[10].data(),&command,previous,&zero);check(output[10][0]==1,"完整任务提交失败");
    }
    float query(int kind){float out=0,item=static_cast<float>(kind),zero=0;functions[6](1,&out,&item,&zero,&zero);return out;}
    void drain(){const auto limit=Clock::now()+std::chrono::seconds(30);while(query(0)>0){check(Clock::now()<limit,"基准任务未能完成");std::this_thread::sleep_for(std::chrono::milliseconds(1));}check(query(3)==0,"后台保存发生错误");}
    fs::path csv(int code,const char *period="1d"){return dir/"csv"/(std::to_string(code)+".SH."+period+".csv");}
};
// 事件单位已更改：只要求行情逐字节相同，两版事件分别按各自约定核对。
void verify_events(const std::string &old_csv,const std::string &new_csv,const Input &input){
    std::istringstream old_stream(old_csv),new_stream(new_csv);std::string old_line,new_line;
    check(bool(std::getline(old_stream,old_line))&&bool(std::getline(new_stream,new_line))&&old_line==new_line,"事件表头不一致");
    for(int i=0;i<input.n;++i){
        check(bool(std::getline(old_stream,old_line))&&bool(std::getline(new_stream,new_line)),"事件提取后日期行丢失");
        const auto old_last=old_line.rfind(','),new_last=new_line.rfind(',');const auto old_first=old_line.rfind(',',old_last-1),new_first=new_line.rfind(',',new_last-1);
        check(old_line.substr(0,old_first)==new_line.substr(0,new_first),"两版行情内容不一致");
        const float actual_old[]={std::stof(old_line.substr(old_first+1,old_last-old_first-1)),std::stof(old_line.substr(old_last+1))};
        const float actual_new[]={std::stof(new_line.substr(new_first+1,new_last-new_first-1)),std::stof(new_line.substr(new_last+1))};
        for(int field=0;field<2;++field){
            const auto expected_old=i&&input.scaled[field][i]!=input.scaled[field][i-1]?input.scaled[field][i]:0.0f;
            const auto expected_new=input.bars[field][i]==0?(field?input.raw[field][i]*10.0f:input.raw[field][i]):0.0f;
            check(actual_old[field]==expected_old,"历史基准事件提取错误");check(actual_new[field]==expected_new,"当前版计数判断或事件单位错误");
        }
    }
    check(!std::getline(old_stream,old_line)&&!std::getline(new_stream,new_line),"额外事件行");
}
void correctness(Module &baseline,Module &paired){
    Input data(7);data.raw[0]={.125f,.25f,.25f,.375f,.375f,.5f,.625f};data.raw[1]={.125f,.125f,.25f,.25f,.25f,.375f,.375f};data.scale();
    data.bars[0]={0,0,1,0,1,0,0};data.bars[1]={1,0,1,0,1,0,1};
    for(auto module:{&baseline,&paired}){
        module->prepare(data,600000);module->receive(data);
        if(module->paired)for(int slot=3;slot<=7;++slot)check(std::all_of(module->output[slot].begin(),module->output[slot].end(),[](float v){return v==-999;}),"合并接口不应写入返回数组");
        module->submit(data);module->drain();
    }
    verify_events(read(baseline.csv(600000)),read(paired.csv(600000)),data);
    for(auto module:{&baseline,&paired}){
        module->prepare(data,600001);module->receive(data,false);module->submit(data,false);module->drain();
        module->prepare(data,600002,2);module->receive(data);module->submit(data);module->drain();
    }
    check(read(baseline.csv(600001))==read(paired.csv(600001)),"不含事件时行情映射错误");
    check(read(baseline.csv(600002,"15m"))==read(paired.csv(600002,"15m")),"分钟事件空列规则改变");
    paired.prepare(data,600003);float kind=1;paired.functions[3](data.n,paired.output[3].data(),&kind,data.series[0].data(),nullptr);
    float command=3,zero=0;paired.functions[1](data.n,paired.output[10].data(),&command,paired.ones.data(),&zero);check(paired.output[10][0]==-1&&!fs::exists(paired.csv(600003)),"成对数组不完整仍允许保存");
    float out=0;paired.functions[7](1,&out,&zero,&zero,&zero);check(out==-1,"接口错误必须由汇总报告");
    std::cout<<"正确性通过：行情逐字节一致、事件按各版单位核对、同值重复事件、分钟空列、不写返回数组及不完整任务保护。\n";
}
double percentile(std::vector<double> values,double q){std::sort(values.begin(),values.end());return values[static_cast<size_t>((values.size()-1)*q)];}
struct Samples {std::vector<double> receive,front,scale;};
void benchmark(Module &baseline,Module &paired,const fs::path &root){
    std::ofstream results(root/"paired-api-results.csv",std::ios::binary);results<<"rows,variant,round,receive_us,frontend_us,scale_us\n";
    std::cout<<std::fixed<<std::setprecision(3);
    for(int n:{512,4096,16384}){
        Input input(n);Samples measured[2];const int repeats=n==512?200:n==4096?100:40;const int code=610000+n;
        for(auto module:{&baseline,&paired}){module->prepare(input,code);module->receive(input);module->submit(input);module->drain();}
        for(int round=0;round<9;++round){
            for(int order=0;order<2;++order){
                const int which=(round+order)%2;auto &module=which?paired:baseline;double receive=0;
                // 单独测量数据接收；开始、预留额度、分配及CSV不计入此段。
                for(int i=0;i<repeats;++i){module.prepare(input,code);const auto begin=Clock::now();module.receive(input);receive+=micros(begin);}
                module.submit(input);module.drain();
                double front=0,scale=0;
                for(int i=0;i<5;++i){
                    const auto begin=Clock::now();module.prepare(input,code);const auto scaling=Clock::now();if(!which)input.scale();scale+=micros(scaling);
                    module.receive(input);module.submit(input);front+=micros(begin);module.drain();
                }
                measured[which].receive.push_back(receive/repeats);measured[which].front.push_back(front/5);measured[which].scale.push_back(scale/5);
                results<<n<<','<<(which?"paired":"baseline")<<','<<round<<','<<std::setprecision(9)<<receive/repeats<<','<<front/5<<','<<scale/5<<'\n';results.flush();
            }
        }
        verify_events(read(baseline.csv(code)),read(paired.csv(code)),input);
        for(int which=0;which<2;++which)std::cout<<"rows="<<n<<" variant="<<(which?"paired":"baseline")<<" receive_median_us="<<percentile(measured[which].receive,.5)<<" receive_p10_us="<<percentile(measured[which].receive,.1)<<" receive_p90_us="<<percentile(measured[which].receive,.9)<<" frontend_median_us="<<percentile(measured[which].front,.5)<<" scale_median_us="<<percentile(measured[which].scale,.5)<<'\n';
    }
}
int main(int argc,char **argv){
    SetConsoleOutputCP(CP_UTF8);
    if(argc!=5)return 2;
    DWORD_PTR allowed=0,system=0;
    if(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system)&&allowed)
        SetThreadAffinityMask(GetCurrentThread(),allowed&(~allowed+1)); // 仅固定基准驱动线程，不改变通达信或后台。
    try{Module baseline(fs::absolute(argv[1]),false),paired(fs::absolute(argv[2]),true);correctness(baseline,paired);if(std::string(argv[4])!="check")benchmark(baseline,paired,fs::absolute(argv[3]));return 0;}
    catch(const std::exception &error){std::cerr<<"失败："<<error.what()<<'\n';return 1;}
}
