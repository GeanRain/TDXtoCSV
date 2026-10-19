#pragma once
#include "../SaveAsCSV/export/async_writer.h"
#include <cstring>
#include <stdexcept>
#include <utility>
namespace tdx_export::ipc
{
constexpr std::uint32_t magic=0x31584353, version=1;
constexpr std::size_t control_limit=4096, data_limit=65536;
enum class Command:std::uint32_t {Hello=1,Start,Reserve,Release,Commit,Receipt,Query,Cancel,Drain,FinalAck,Data,DataEnd,TaskStatus,Begin};
enum class Result:std::uint32_t {Ok,Wait,Rejected,Ready,Accepted,Done};
struct Handle
{
    HANDLE value=nullptr;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    ~Handle(){reset();}
    Handle(const Handle &)=delete;Handle &operator=(const Handle &)=delete;
    Handle(Handle &&other) noexcept:value(std::exchange(other.value,nullptr)){}
    Handle &operator=(Handle &&other) noexcept {if(this!=&other){reset();value=std::exchange(other.value,nullptr);}return *this;}
    explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
    void reset(HANDLE h=nullptr){if(*this)CloseHandle(value);value=h;}
};
struct Buffer
{
    std::vector<unsigned char> data;
    void u32(std::uint32_t v){for(int i=0;i<4;++i){data.push_back(static_cast<unsigned char>(v));v>>=8;}}
    void u64(std::uint64_t v){for(int i=0;i<8;++i){data.push_back(static_cast<unsigned char>(v));v>>=8;}}
    void real(double v){std::uint64_t bits;std::memcpy(&bits,&v,8);u64(bits);}
    void str(const std::string &v){if(v.size()>control_limit)throw std::runtime_error("管道字符串过长");u32(static_cast<std::uint32_t>(v.size()));data.insert(data.end(),v.begin(),v.end());}
};
struct Reader
{
    const unsigned char *data;const std::size_t size;std::size_t at=0;
    Reader(const unsigned char *bytes,std::size_t length):data(bytes),size(length){}
    explicit Reader(const std::vector<unsigned char> &b):Reader(b.data(),b.size()){}
    void need(std::size_t n)const{if(n>size-at)throw std::runtime_error("管道消息截断");}
    std::uint32_t u32(){need(4);std::uint32_t v=0;for(int i=0;i<4;++i)v|=std::uint32_t(data[at++])<<(i*8);return v;}
    std::uint64_t u64(){need(8);std::uint64_t v=0;for(int i=0;i<8;++i)v|=std::uint64_t(data[at++])<<(i*8);return v;}
    double real(){auto bits=u64();double v;std::memcpy(&v,&bits,8);return v;}
    std::string str(std::size_t maximum=control_limit){const auto n=u32();need(n);if(n>maximum)throw std::runtime_error("管道字符串超限");std::string s(reinterpret_cast<const char *>(data+at),n);at+=n;if(s.find('\0')!=std::string::npos)throw std::runtime_error("管道字符串含NUL");return s;}
    bool done()const{return at==size;}
    void end()const{if(!done())throw std::runtime_error("管道消息含未定义尾部");}
};
struct Packet {Command command=Command::Hello;std::uint64_t epoch=0,id=0;Buffer body;};
std::uint32_t crc32(std::uint32_t previous,const void *data,std::size_t size) noexcept;
void encode_status(Buffer &b,const WriterStatus &s);
WriterStatus decode_status(Reader &r);
void encode_job(Buffer &b,const ExportJob &job);
ExportJob decode_job(Reader &r);
std::vector<float> &array(ExportJob &job,unsigned field);
std::wstring wide(const std::string &text);
std::string narrow(const std::wstring &text);
std::wstring relative_executable(const std::wstring &directory,const std::string &relative);
std::uint64_t creation_time(HANDLE process);
std::wstring pipe_base(DWORD pid,std::uint64_t created,const std::wstring &directory);
[[noreturn]] void win_error(const char *operation);

// 每条连接复用事件；所有在途操作结束后才能销毁缓冲或连接。
class Channel
{
    Handle pipe_,event_;
    void transfer(void *data,std::size_t size,bool writing,HANDLE stop,HANDLE peer,DWORD timeout);
    void pending(OVERLAPPED &operation,HANDLE stop,HANDLE peer,DWORD timeout);
public:
    Channel()=default;explicit Channel(HANDLE pipe):pipe_(pipe),event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)){if(!event_)win_error("创建管道事件");}
    Channel(Channel &&)=default;Channel &operator=(Channel &&)=default;
    explicit operator bool()const{return bool(pipe_);}
    HANDLE handle()const{return pipe_.value;}
    void close(){pipe_.reset();event_.reset();}
    void accept(HANDLE stop,HANDLE peer);
    void send(const Packet &packet,HANDLE stop=nullptr,HANDLE peer=nullptr,DWORD timeout=5000);
    Packet receive(std::size_t limit=control_limit,HANDLE stop=nullptr,HANDLE peer=nullptr,DWORD timeout=5000);
};
Handle create_pipe(const std::wstring &name);
}
