// 仅链接到测试Worker：可控保存屏障、真实new失败和命令执行后的丢回包。
#include "../shared/ipc.h"
#include "../shared/csv_store.h"
#include <atomic>
#include <cstdlib>
#include <new>
namespace {thread_local int allocation_failures=0;}
void *operator new(std::size_t size){if(allocation_failures>0){--allocation_failures;throw std::bad_alloc();}if(auto p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void *operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void *p)noexcept{std::free(p);}void operator delete[](void *p)noexcept{std::free(p);}
void operator delete(void *p,std::size_t)noexcept{std::free(p);}void operator delete[](void *p,std::size_t)noexcept{std::free(p);}
namespace {
int setting(const char *name){char value[16]{};return GetEnvironmentVariableA(name,value,sizeof(value))?std::atoi(value):0;}
}
bool test_save(tdx_export::ExportJob &job){
    char name[128]{};if(GetEnvironmentVariableA("TDX_TEST_GATE",name,sizeof(name))){
        tdx_export::ipc::Handle gate(OpenEventA(SYNCHRONIZE,FALSE,name));if(!gate)throw std::runtime_error("测试保存屏障不存在");WaitForSingleObject(gate.value,INFINITE);
    }
    allocation_failures=setting("TDX_TEST_ALLOC_FAILURES");return tdx_export::perform_save(job);
}
bool test_drop_reply(tdx_export::ipc::Command command){
    using tdx_export::ipc::Command;
    static std::atomic<int> reserve{setting("TDX_TEST_DROP_RESERVE")},commit{setting("TDX_TEST_DROP_COMMIT")},final_ack{setting("TDX_TEST_DROP_FINAL")};
    auto n=command==Command::Reserve?&reserve:command==Command::Commit?&commit:command==Command::FinalAck?&final_ack:nullptr;
    return n&&n->fetch_sub(1)>0;
}

// 仅测试版本在完整临时CSV刷盘后、原子替换前暂停，验证强制退出保护旧文件。
BOOL WINAPI test_replace_csv(LPCSTR source,LPCSTR target,DWORD flags){
    char name[128]{};
    if(GetEnvironmentVariableA("TDX_TEST_REPLACE_GATE",name,sizeof(name))){
        tdx_export::ipc::Handle gate(OpenEventA(SYNCHRONIZE,FALSE,name));if(!gate)throw std::runtime_error("替换屏障不存在");
        char ready[128]{};if(!GetEnvironmentVariableA("TDX_TEST_REPLACE_READY",ready,sizeof(ready)))throw std::runtime_error("替换就绪事件缺失");
        tdx_export::ipc::Handle signal(OpenEventA(EVENT_MODIFY_STATE,FALSE,ready));if(!signal)throw std::runtime_error("替换就绪事件不存在");SetEvent(signal.value);
        WaitForSingleObject(gate.value,INFINITE);
    }
    return MoveFileExA(source,target,flags);
}
#define MoveFileExA test_replace_csv
#include "../shared/csv_store.cpp"
#undef MoveFileExA
