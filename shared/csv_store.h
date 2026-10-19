#pragma once
#include "../SaveAsCSV/export/async_writer.h"
namespace tdx_export
{
constexpr int kMaxCount = 2000000;
double elapsed_ms(ExportClock::time_point begin);
bool integer(float value, int lower, int upper, int &result);
std::string join_path(const std::string &base, const std::string &name);
bool valid_time(int time);
std::string period_name(int code);
bool write_csv(ExportJob &job);
bool perform_save(ExportJob &job) noexcept;
}
