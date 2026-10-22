#include "ipc.h"
#include "csv_store.h"
#include "../SaveAsCSV/utils/logger.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <sddl.h>
namespace tdx_export::ipc
{
[[noreturn]] void win_error(const char *operation)
{
    const auto error=GetLastError();
    if(error==ERROR_NOT_ENOUGH_MEMORY||error==ERROR_OUTOFMEMORY)throw std::bad_alloc();
    throw std::runtime_error(std::string(operation)+" Windows="+std::to_string(error));
}
std::uint32_t crc32(std::uint32_t previous,const void *bytes,std::size_t size) noexcept
{
    static const auto table=[]{std::array<std::uint32_t,256> result{};for(unsigned i=0;i<256;++i){auto c=i;for(int bit=0;bit<8;++bit)c=(c>>1)^(0xedb88320U&static_cast<std::uint32_t>(-static_cast<int>(c&1)));result[i]=c;}return result;}();
    auto c=~previous;auto data=static_cast<const unsigned char *>(bytes);
    for(std::size_t i=0;i<size;++i)c=table[(c^data[i])&255]^(c>>8);
    return ~c;
}
void encode_status(Buffer &b,const WriterStatus &s)
{
    for(auto v:{s.accepted,s.succeeded,s.failed,s.rejected,s.reserved,s.unfinished,s.bytes,s.waiting,s.busy})b.u64(v);
    b.u32(s.running?1:0);b.u32(s.accepting?1:0);
}
WriterStatus decode_status(Reader &r)
{
    WriterStatus s;s.accepted=r.u64();s.succeeded=r.u64();s.failed=r.u64();s.rejected=r.u64();s.reserved=r.u64();s.unfinished=r.u64();s.bytes=r.u64();s.waiting=r.u64();s.busy=r.u64();s.running=r.u32()!=0;s.accepting=r.u32()!=0;return s;
}
void encode_job(Buffer &b,const ExportJob &j)
{
    b.u32(j.count);b.u32(j.native_period);b.u32(j.default_time);b.u32(j.minute?1:0);b.u32(j.caller_thread);
    b.str(j.symbol);b.str(j.period);b.str(j.csv_dir);
    for(double v:j.call_ms)b.real(v);
}
ExportJob decode_job(Reader &r)
{
    ExportJob j;const auto count=r.u32(),period=r.u32(),time=r.u32(),minute=r.u32();j.caller_thread=r.u32();
    if(!count||count>kMaxCount||period>11||time>235959||minute>1)throw std::runtime_error("后台任务元数据无效");
    j.count=static_cast<int>(count);j.native_period=static_cast<int>(period);j.default_time=static_cast<int>(time);j.minute=minute!=0;
    j.symbol=r.str(32);j.period=r.str(8);j.csv_dir=r.str(MAX_PATH-1);
    const auto dot=j.symbol.find('.');
    if(dot==std::string::npos||!dot||dot>12||j.symbol.find('.',dot+1)!=std::string::npos||
       !std::all_of(j.symbol.begin(),j.symbol.begin()+dot,[](char c){return c>='0'&&c<='9';})||
       !std::all_of(j.symbol.begin()+dot+1,j.symbol.end(),[](char c){return c>='A'&&c<='Z';})||dot+1==j.symbol.size()||
       j.period!=period_name(j.native_period)||j.period.empty()||j.minute!=(j.native_period<=4)||!valid_time(j.default_time)||
       j.csv_dir.empty()||!(j.csv_dir.size()>2&&j.csv_dir[1]==':'&&(j.csv_dir[2]=='\\'||j.csv_dir[2]=='/')||j.csv_dir.rfind("\\\\",0)==0))
        throw std::runtime_error("后台任务代码、周期或绝对路径无效");
    for(double &v:j.call_ms){v=r.real();if(!std::isfinite(v)||v<0)throw std::runtime_error("后台任务计时无效");}
    j.started=ExportClock::now();return j;
}
std::uint64_t creation_time(HANDLE process)
{
    FILETIME created,exited,kernel,user;if(!GetProcessTimes(process,&created,&exited,&kernel,&user))win_error("读取进程创建时间");
    return (std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
std::vector<float> &array(ExportJob &j,unsigned field)
{
    if(field==0)return j.dates;if(field==1)return j.times;
    if(field<8)return j.series[field-2];throw std::runtime_error("行情数组编号无效");
}
std::wstring wide(const std::string &text)
{
    if(text.empty())return {};
    const auto size=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!size)win_error("转换本地路径");std::wstring result(size,L'\0');
    if(!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size))win_error("转换本地路径");return result;
}
std::string narrow(const std::wstring &text)
{
    if(text.empty())return {};
    BOOL used=FALSE;const auto utf8=GetACP()==CP_UTF8;auto fallback=utf8?nullptr:&used;
    const auto flags=utf8?WC_ERR_INVALID_CHARS:WC_NO_BEST_FIT_CHARS;
    const auto size=WideCharToMultiByte(CP_ACP,flags,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,fallback);
    if(!size||used)throw std::runtime_error("路径不能以本地代码页无损表示");std::string result(size,'\0');
    if(!WideCharToMultiByte(CP_ACP,flags,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,fallback)||used)throw std::runtime_error("路径转换失败");return result;
}
std::wstring pipe_base(DWORD pid,std::uint64_t created,const std::wstring &directory,std::uint64_t lifetime)
{
    std::uint64_t hash=14695981039346656037ULL;
    for(auto c:directory){if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';if(c==L'/')c=L'\\';hash^=static_cast<std::uint16_t>(c);hash*=1099511628211ULL;}
    auto base=L"\\\\.\\pipe\\SaveAsCSV_"+std::to_wstring(pid)+L"_"+std::to_wstring(created)+L"_"+std::to_wstring(hash);
    if(lifetime)base+=L"_"+std::to_wstring(lifetime);
    return base;
}
std::wstring relative_executable(const std::wstring &directory,const std::string &relative)
{
    if(relative.empty()||relative.size()>=MAX_PATH||relative.front()=='\\'||relative.front()=='/'||
       relative.find(':')!=std::string::npos||relative.find('\0')!=std::string::npos)
        throw std::runtime_error("worker_path 必须填写相对于DLL目录的非空相对路径");
    const auto joined=directory+L"\\"+wide(relative);wchar_t absolute[MAX_PATH]{};
    const auto size=GetFullPathNameW(joined.c_str(),MAX_PATH,absolute,nullptr);
    if(!size||size>=MAX_PATH)throw std::runtime_error("后台相对路径解析失败或过长");
    return std::wstring(absolute,size);
}
void Channel::pending(OVERLAPPED &operation,HANDLE stop,HANDLE peer,DWORD timeout)
{
    HANDLE events[3]={event_.value};DWORD count=1;if(stop)events[count++]=stop;if(peer)events[count++]=peer;
    const auto begin=GetTickCount64();
    for(;;){
        const auto result=WaitForMultipleObjects(count,events,FALSE,200);
        if(result==WAIT_OBJECT_0)return;
        if(result!=WAIT_TIMEOUT||(timeout&&GetTickCount64()-begin>=timeout)){
            CancelIoEx(pipe_.value,&operation);DWORD transferred=0;GetOverlappedResult(pipe_.value,&operation,&transferred,TRUE);
            throw std::runtime_error(result==WAIT_TIMEOUT?"管道操作超时，提交结果须核实":"管道操作停止或对端进程结束");
        }
    }
}
void Channel::transfer(void *bytes,std::size_t size,bool writing,HANDLE stop,HANDLE peer,DWORD timeout)
{
    auto data=static_cast<unsigned char *>(bytes);
    while(size){
        OVERLAPPED operation{};operation.hEvent=event_.value;ResetEvent(event_.value);DWORD done=0;
        const auto request=static_cast<DWORD>(std::min(size,data_limit));
        BOOL ok=writing?WriteFile(pipe_.value,data,request,&done,&operation):ReadFile(pipe_.value,data,request,&done,&operation);
        if(!ok){if(GetLastError()!=ERROR_IO_PENDING)win_error("管道读写");pending(operation,stop,peer,timeout);if(!GetOverlappedResult(pipe_.value,&operation,&done,FALSE))win_error("管道完成");}
        if(!done||done>size)throw std::runtime_error("管道已经断开或返回无效长度");
        data+=done;size-=done;
    }
}
void Channel::accept(HANDLE stop,HANDLE peer)
{
    OVERLAPPED operation{};operation.hEvent=event_.value;ResetEvent(event_.value);
    if(ConnectNamedPipe(pipe_.value,&operation))return;
    const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return;if(error!=ERROR_IO_PENDING)win_error("等待管道连接");
    pending(operation,stop,peer,0);DWORD done;if(!GetOverlappedResult(pipe_.value,&operation,&done,FALSE))win_error("完成管道连接");
}
void Channel::send(const Packet &packet,HANDLE stop,HANDLE peer,DWORD timeout)
{
    if(packet.body.data.size()>data_limit)throw std::runtime_error("管道帧过长");
    unsigned char header[32]{};std::size_t at=0;
    auto number=[&](std::uint64_t value,int bytes){while(bytes--){header[at++]=static_cast<unsigned char>(value);value>>=8;}};
    number(magic,4);number(version,4);number(static_cast<std::uint32_t>(packet.command),4);number(packet.body.data.size(),4);number(packet.epoch,8);number(packet.id,8);
    transfer(header,sizeof(header),true,stop,peer,timeout);
    transfer(const_cast<unsigned char *>(packet.body.data.data()),packet.body.data.size(),true,stop,peer,timeout);
}
Packet Channel::receive(std::size_t limit,HANDLE stop,HANDLE peer,DWORD timeout)
{
    unsigned char header[32]{};transfer(header,sizeof(header),false,stop,peer,timeout);Reader r(header,sizeof(header));
    if(r.u32()!=magic||r.u32()!=version)throw std::runtime_error("管道标记或协议版本不匹配");
    Packet p;p.command=static_cast<Command>(r.u32());const auto size=r.u32();p.epoch=r.u64();p.id=r.u64();
    if(size>limit||size>data_limit)throw std::runtime_error("管道正文超出限制");
    // 头已读完时不能因一次分配失败跳过正文；保留当前帧并重试，避免破坏流边界。
    auto next_report=GetTickCount64();
    for(;;){try{p.body.data.resize(size);break;}catch(const std::bad_alloc &){
        if(GetTickCount64()>=next_report){next_report=GetTickCount64()+5000;try{Logger::log("WARN","管道正文内存分配重试 id=%llu bytes=%lu retry_ms=200",p.id,size);}catch(...){};}
        if((stop&&WaitForSingleObject(stop,0)==WAIT_OBJECT_0)||(peer&&WaitForSingleObject(peer,0)!=WAIT_TIMEOUT))throw;
        if(stop)WaitForSingleObject(stop,200);else Sleep(200);
    }}
    transfer(p.body.data.data(),size,false,stop,peer,timeout);return p;
}
Handle create_pipe(const std::wstring &name)
{
    Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))win_error("读取管道用户");
    DWORD size=0;GetTokenInformation(token.value,TokenGroups,nullptr,0,&size);std::vector<unsigned char> bytes(size);
    if(!GetTokenInformation(token.value,TokenGroups,bytes.data(),size,&size))win_error("读取登录会话");
    auto groups=reinterpret_cast<TOKEN_GROUPS *>(bytes.data());PSID logon=nullptr;
    for(DWORD i=0;i<groups->GroupCount;++i)if((groups->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID)logon=groups->Groups[i].Sid;
    if(!logon)throw std::runtime_error("无法识别本地登录会话");
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(logon,&sid))win_error("转换登录会话");
    std::wstring sddl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);PSECURITY_DESCRIPTOR security=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&security,nullptr))win_error("创建管道权限");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};
    Handle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&attributes));
    const auto error=GetLastError();LocalFree(security);if(!pipe){SetLastError(error);win_error("创建本机管道");}return pipe;
}
}
