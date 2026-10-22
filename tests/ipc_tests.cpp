// 管道协议回归：二进制无损、长度边界和已知 CRC，不耗尽实际宿主资源。
#include "../shared/ipc.h"
#include "../SaveAsCSV/utils/logger.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace tdx_export::ipc;
void Logger::log(const char *,const char *,...){}
int main(){
    int passed=0,failed=0;
    auto check=[&](bool ok,const char *text){std::cout<<(ok?"通过 ":"失败 ")<<text<<'\n';ok?++passed:++failed;};
    try{
        Buffer b;b.u32(0x89abcdef);b.u64(0xfedcba9876543210ULL);b.str("600000.SH");b.real(0.125);
        Reader r(b.data);check(r.u32()==0x89abcdef&&r.u64()==0xfedcba9876543210ULL&&r.str()=="600000.SH"&&r.real()==0.125&&r.done(),"固定宽度字段往返无损");
        check(crc32(0,"123456789",9)==0xcbf43926,"CRC32匹配公开已知向量");
        check(pipe_base(1,2,L"D:\\tdx",3)!=pipe_base(1,2,L"D:\\tdx",4),"不同DLL装载周期使用独立管道");
        check(pipe_base(1,2,L"D:\\tdx")!=pipe_base(1,2,L"D:\\tdx",3),"DLL管道与无生命周期的独立客户端隔离");
        auto crc=crc32(0,"1234",4);check(crc32(crc,"56789",5)==0xcbf43926,"分块CRC32与完整计算一致");
        bool denied=false;try{r.u32();}catch(const std::exception &){denied=true;}check(denied,"拒绝截断字段");
        Buffer invalid;invalid.u32(4096);Reader bad(invalid.data);denied=false;try{bad.str();}catch(const std::exception &){denied=true;}check(denied,"拒绝超出消息正文的字符串长度");
        Buffer nul;nul.str(std::string("a\0b",3));Reader zero(nul.data);denied=false;try{zero.str();}catch(const std::exception &){denied=true;}check(denied,"拒绝路径字符串中的NUL");
        Buffer oversized;oversized.str(std::string(300,'x'));Reader path(oversized.data);denied=false;try{path.str(259);}catch(const std::exception &){denied=true;}check(denied,"拒绝超出约定路径长度的字符串");
        check(relative_executable(L"D:\\tdx\\dlls","backend\\SaveAsCSVWorker.exe")==L"D:\\tdx\\dlls\\backend\\SaveAsCSVWorker.exe","后台子目录路径相对于DLL解析");
        check(relative_executable(L"D:\\tdx\\dlls","..\\worker\\SaveAsCSVWorker.exe")==L"D:\\tdx\\worker\\SaveAsCSVWorker.exe","后台父目录相对路径规范化");
        for(const std::string input:{"C:\\worker.exe","C:worker.exe","\\\\server\\worker.exe","\\worker.exe","/worker.exe",""}){
            denied=false;try{relative_executable(L"D:\\tdx\\dlls",input);}catch(const std::exception &){denied=true;}check(denied,"拒绝绝对、驱动器相对或空后台路径");
        }
    }catch(const std::exception &e){std::cout<<"异常："<<e.what()<<'\n';++failed;}
    std::cout<<"协议回归："<<passed<<" 通过，"<<failed<<" 失败\n";return failed?1:0;
}
