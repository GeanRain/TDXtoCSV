// CSV转换优化的差分回归：旧流实现作为参考，接受范围与浮点位模式必须一致。
#include "../shared/csv_store.cpp"
#include <cstring>
#include <iostream>
#include <random>
#include <filesystem>
using namespace tdx_export;
void Logger::log(const char *,const char *,...){}
bool old_number(const std::string &text,float &number){
    const auto raw=trim(text);std::istringstream input(raw);input.exceptions(std::ios::badbit);input.imbue(std::locale::classic());double parsed=0;input>>std::noskipws>>parsed;
    if(!input||input.peek()!=std::char_traits<char>::eof()||!std::isfinite(parsed)||parsed>FLT_MAX||parsed<-FLT_MAX)return false;
    number=static_cast<float>(parsed);return std::isfinite(number)&&(number!=0||raw.substr(0,raw.find_first_of("eE")).find_first_of("123456789")==std::string::npos);
}
bool old_row(const std::string &line,size_t column_count,CsvRow &row,const ExportJob &job){
    std::vector<std::string> columns;std::istringstream input(line);std::string part;while(std::getline(input,part,','))columns.push_back(trim(part));if(!line.empty()&&line.back()==',')columns.push_back("");if(columns.size()!=column_count)return false;
    row.symbol=columns[0];row.period=columns[1];row.ts=columns[2];if(row.symbol!=job.symbol||row.period!=job.period||!parse_timestamp(row.ts,row.date,job))return false;
    for(int i=0;i<6;++i)if(!old_number(columns[i+3],row.values[i]))return false;row.key=row_key(row.period,row.date,row.ts);return valid_values(row);
}
void check(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(){
    SetConsoleOutputCP(CP_UTF8);try{
        std::vector<std::string> corpus={""," ","+","-","+-1","++1","--1","1 2","1x","0x1","1,2","nan","NaN","inf","Infinity","1.#INF","1e","1e+",".5","1.","+1.2e+03","-0","+0e999999","0e-999999","1e-100","-1e-400","1e400","3.40282e38","3.402824e38","1.401298464324817e-45","7.006492321624085e-46","\v1","\f1","\t 10 \r\n"};
        std::mt19937 random(20261020);for(int i=0;i<10000;++i){const auto bits=static_cast<std::uint32_t>(random());float value;std::memcpy(&value,&bits,4);if(!std::isfinite(value))continue;std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(i%2?6:17)<<static_cast<double>(value);corpus.push_back(text.str());if(i%10==0)corpus.push_back(" \t+"+text.str()+"\r\n");}
        for(const auto &text:corpus){float a=0,b=0;const auto before=old_number(text,a),after=parse_number(text,b);if(before!=after||(before&&std::memcmp(&a,&b,4)!=0)){std::cerr<<"数值语义不一致："<<text<<'\n';return 1;}}
        std::cout<<"通过 数值差分 "<<corpus.size()<<" 项，包含空白、符号、指数、上下溢与有符号零\n";
        for(int year:{1900,1999,2000,2020,2026,2099,2400,9999})for(int month=1;month<=12;++month)for(int day:{1,15,28})for(int time:{0,93000,150000,235959}){
            const auto date=year*10000+month*100+day;std::ostringstream text;text.imbue(std::locale::classic());text<<std::setfill('0')<<std::setw(4)<<year<<'-'<<std::setw(2)<<month<<'-'<<std::setw(2)<<day<<' '<<std::setw(2)<<time/10000<<':'<<std::setw(2)<<time/100%100<<':'<<std::setw(2)<<time%100;check(format_ts(date,time)==text.str(),"时间格式不一致");
        }
        std::cout<<"通过 1152 项时间格式与旧版逐字节一致\n";
        for(const auto &text:corpus){const ExportJob job{1,5,150000,false,"600000.SH","1d"};for(const auto &suffix:{std::string(),std::string(","),std::string(",extra"),std::string(",extra,")}){
            const auto line=" 600000.SH , 1d , 2026-10-20 15:00:00+08 ,10,12,9,11, "+text+",1000"+suffix;CsvRow a,b;const size_t columns=suffix.empty()?9:10;const auto before=old_row(line,columns,a,job),after=parse_csv_row(line,columns,b,job);check(before==after,"CSV列接受范围改变");if(before)check(a.symbol==b.symbol&&a.period==b.period&&a.ts==b.ts&&a.key==b.key&&a.date==b.date&&std::memcmp(a.values,b.values,sizeof(a.values))==0,"CSV解析值改变");
        }}
        std::cout<<"通过 CSV列与数值组合差分，保留空尾列及旧周期扩展列语义\n";
        ExportJob job;job.symbol="600000.SH";job.period="1d";job.native_period=5;job.count=10000;
        const auto directory=std::filesystem::current_path()/("conversion-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));job.csv_dir=directory.string();
        std::ostringstream expected;expected.imbue(std::locale::classic());expected<<kHeader<<'\n';int year=2020,month=1,day=1;
        const float edges[]={0.0f,-0.0f,FLT_MAX,-FLT_MAX,std::numeric_limits<float>::denorm_min(),-std::numeric_limits<float>::denorm_min(),1e-5f,1e-4f,999999.5f,1000000.f};
        for(int i=0;i<job.count;++i){
            auto bits=static_cast<std::uint32_t>(random());float value;std::memcpy(&value,&bits,4);if(!std::isfinite(value))value=0;if(i<10)value=edges[i];const auto positive=std::fabs(value);
            const auto date=year*10000+month*100+day;job.dates.push_back(static_cast<float>(date-19000000));for(int field=0;field<6;++field)job.series[field].push_back(field<4?value:positive);
            expected<<job.symbol<<",1d,"<<format_ts(date,150000);for(int field=0;field<6;++field)expected<<','<<(field<4?value:positive);expected<<'\n';
            if(!valid_date(year*10000+month*100+(++day))){day=1;if(++month>12){month=1;++year;}}
        }
        check(write_csv(job),"随机极值保存失败");std::ifstream input(directory/"600000.SH.1d.csv",std::ios::binary);const std::string actual{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};check(actual==expected.str(),"六位有效数字输出与旧流格式不一致");
        std::cout<<"通过 一万行随机浮点及极值输出逐字节一致，含科学计数法和负零\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
