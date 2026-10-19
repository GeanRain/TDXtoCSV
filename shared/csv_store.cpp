// CSV 保存核心：由 DLL 回归及独立后台共用，文件保护流程保持一致。
#include "csv_store.h"
#include "../SaveAsCSV/common.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace tdx_export
{
constexpr LONGLONG kMaxCsvBytes = 512LL * 1024 * 1024;
const char *kHeader = "symbol,period,ts,open,high,low,close,volume,amount";
double elapsed_ms(ExportClock::time_point begin)
{
    return std::chrono::duration<double, std::milli>(ExportClock::now() - begin).count();
}

struct CsvRow
{
    std::string symbol;
    std::string period;
    std::string ts;
    std::string key;
    int date = 0;
    float values[6]{}; // CSV 顺序：开、高、低、收、量、额
};

struct FileHandle
{
    HANDLE value;
    explicit FileHandle(HANDLE handle) : value(handle) {}
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;
};

struct TempFile
{
    char path[MAX_PATH]{}; // 创建后立即持有文件名，内存紧张时也能可靠清理。
    ~TempFile() { if (path[0]) DeleteFileA(path); }
};

bool integer(float value, int lower, int upper, int &result)
{
    if (!std::isfinite(value) || static_cast<double>(value) < lower ||
        static_cast<double>(value) > upper || std::floor(value) != value)
        return false;
    result = static_cast<int>(value);
    return true;
}

// 即使读取失败或抛出异常，也记录已执行阶段；尚未执行的阶段保持为零。
struct SaveTiming
{
    double (&values)[4];
    int phase = 0;
    ExportClock::time_point begin = ExportClock::now();
    explicit SaveTiming(double (&durations)[4]) : values(durations) {}
    void next()
    {
        const auto end = ExportClock::now();
        values[phase++] += std::chrono::duration<double, std::milli>(end - begin).count();
        begin = end;
    }
    ~SaveTiming() { values[phase] += elapsed_ms(begin); }
};

void log_export_timing(const ExportJob &s, float result)
{
    if (s.started == ExportClock::time_point{} || s.count <= 0) return;
    double receive = 0;
    for (int i = 0; i < 9; ++i) receive += s.call_ms[i];
    const double save = s.save_call_ms, total = receive + s.call_ms[9] + save;
    const double wall = s.frontend_elapsed_ms + elapsed_ms(s.started);
    const char *status = result == 1.0f ? "ok" : "failed";
    const char *period = s.period.empty() ? "-" : s.period.c_str();
    // 两条日志可相互关联；标识只用于日志，不参与接口或文件命名。
    try
    {
        LOG_INFO("导出耗时 symbol=%s period=%s id=%lu:%llu input=%d result=%s "
                 "receive_ms=%.3f save_ms=%.3f total_ms=%.3f save_pct=%.3f "
                 "submit_ms=%.3f queue_wait_ms=%.3f "
                 "wall_ms=%.3f start_ms=%.3f meta_ms=%.3f date_ms=%.3f "
                 "open_ms=%.3f close_ms=%.3f high_ms=%.3f low_ms=%.3f volume_ms=%.3f amount_ms=%.3f",
                 s.symbol.c_str(), period, s.caller_thread, s.id, s.count, status,
                 receive, save, total, total > 0 ? save * 100 / total : 0,
                 s.call_ms[9], s.queue_wait_ms, wall,
                 s.call_ms[0], s.call_ms[1], s.call_ms[2], s.call_ms[3], s.call_ms[4],
                 s.call_ms[5], s.call_ms[6], s.call_ms[7], s.call_ms[8]);
    }
    catch (...) {} // 计时日志失败不能改变已经提交的保存结果。
    try
    {
        const double scale = save > 0 ? 100 / save : 0;
        LOG_INFO("保存分段 symbol=%s period=%s id=%lu:%llu result=%s "
                 "prepare_ms=%.3f read_ms=%.3f merge_ms=%.3f write_ms=%.3f "
                 "prepare_pct=%.3f read_pct=%.3f merge_pct=%.3f write_pct=%.3f",
                 s.symbol.c_str(), period, s.caller_thread, s.id, status,
                 s.save_ms[0], s.save_ms[1], s.save_ms[2], s.save_ms[3],
                 s.save_ms[0] * scale, s.save_ms[1] * scale,
                 s.save_ms[2] * scale, s.save_ms[3] * scale);
    }
    catch (...) {}
}

std::string join_path(const std::string &base, const std::string &name)
{
    if (base.empty()) return name;
    if (base.back() == '\\' || base.back() == '/') return base + name;
    return base + "\\" + name;
}

bool ensure_dir(const std::string &path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::string current;
    for (char c : path)
    {
        current.push_back(c);
        if (c == '\\' || c == '/') CreateDirectoryA(current.c_str(), nullptr);
    }
    return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string period_name(int code)
{
    switch (code)
    {
    case 0: return "1m";
    case 1: return "5m";
    case 2: return "15m";
    case 3: return "30m";
    case 4: return "60m";
    case 5: return "1d";
    case 6: return "1w";
    case 7: return "1mo";
    case 11: return "1y";
    default: return "";
    }
}

bool leap_year(int year)
{
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

int day_of_year(int year, int month, int day)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 1900 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > days[month - 1] + (month == 2 && leap_year(year) ? 1 : 0))
        return 0;
    int result = day;
    for (int m = 1; m < month; ++m)
        result += days[m - 1] + (m == 2 && leap_year(year) ? 1 : 0);
    return result;
}

bool valid_date(int date)
{
    return day_of_year(date / 10000, date / 100 % 100, date % 100) > 0;
}

bool valid_time(int time)
{
    return time >= 0 && time / 10000 < 24 && time / 100 % 100 < 60 && time % 100 < 60;
}

int weekday(int year, int month, int day)
{
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (month < 3) --year;
    int result = (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
    return result == 0 ? 7 : result;
}

int weeks_in_year(int year)
{
    int jan1 = weekday(year, 1, 1);
    return jan1 == 4 || (jan1 == 3 && leap_year(year)) ? 53 : 52;
}

std::string row_key(const std::string &period, int date, const std::string &ts)
{
    if(period!="1w"&&period!="1mo"&&period!="1y")return ts;
    int year = date / 10000, month = date / 100 % 100, day = date % 100;
    std::ostringstream key;
    key.exceptions(std::ios::badbit);
    key.imbue(std::locale::classic());
    if (period == "1w")
    {
        int week = (day_of_year(year, month, day) - weekday(year, month, day) + 10) / 7;
        if (week < 1) { --year; week = weeks_in_year(year); }
        else if (week > weeks_in_year(year)) { ++year; week = 1; }
        key << std::setfill('0') << std::setw(4) << year << 'W' << std::setw(2) << week;
    }
    else if (period == "1mo")
        key << std::setfill('0') << std::setw(4) << year << std::setw(2) << month;
    else if (period == "1y")
        key << std::setfill('0') << std::setw(4) << year;
    else
        return ts;
    return key.str();
}

std::string key_column(const std::string &period)
{
    if (period == "1w") return "week_key";
    if (period == "1mo") return "month_key";
    if (period == "1y") return "year_key";
    return "";
}

std::string format_ts(int date, int time)
{
    // 调用处已检查日期/时间有效，固定19字节格式无需逐行建立locale和文本流。
    std::string text="0000-00-00 00:00:00";
    const int values[]={date/1000000,date/10000%100,date/100%100,date%100,time/10000,time/100%100,time%100};
    const int positions[]={0,2,5,8,11,14,17};
    for(int i=0;i<7;++i){text[positions[i]]=static_cast<char>('0'+values[i]/10);text[positions[i]+1]=static_cast<char>('0'+values[i]%10);}
    return text;
}

std::string trim(const std::string &text)
{
    size_t first = text.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}

std::string_view trim_view(std::string_view text)
{
    const auto first=text.find_first_not_of(" \r\n\t");
    if(first==std::string_view::npos)return {};
    return text.substr(first,text.find_last_not_of(" \r\n\t")-first+1);
}

bool parse_number(std::string_view text, float &number)
{
    const auto raw=trim_view(text);
    auto digits=raw;
    // from_chars 不接受前导加号，显式兼容原流解析；不能把 "+-1" 变成合法数字。
    if(!digits.empty()&&digits.front()=='+'){
        digits.remove_prefix(1);
        if(!digits.empty()&&(digits.front()=='+'||digits.front()=='-'))return false;
    }
    if(digits.empty())return false;
    double parsed = 0;
    const auto sign=digits.front()=='-'?1u:0u;
    if(digits.size()>sign+1&&digits[sign]=='0'&&(digits[sign+1]=='x'||digits[sign+1]=='X')){
        // 旧 MSVC 流也接受十六进制数；仅此少见格式保留原解析，普通CSV走无分配路径。
        std::istringstream input{std::string(raw)};input.exceptions(std::ios::badbit);input.imbue(std::locale::classic());input>>std::noskipws>>parsed;
        if(!input||input.peek()!=std::char_traits<char>::eof())return false;
    }else{
        const auto result=std::from_chars(digits.data(),digits.data()+digits.size(),parsed,std::chars_format::general);
        if(result.ec!=std::errc{}||result.ptr!=digits.data()+digits.size())return false;
    }
    if(!std::isfinite(parsed)||parsed>FLT_MAX||parsed<-FLT_MAX)return false;
    number = static_cast<float>(parsed);
    // 非零文本转为零属于下溢，不能当作正常的六位有效数字舍入。
    return std::isfinite(number) && (number != 0 ||
           raw.substr(0, raw.find_first_of("eE")).find_first_of("123456789") == std::string_view::npos);
}

bool parse_timestamp(std::string &ts, int &date, const ExportJob &job)
{
    ts = trim(ts);
    if (ts.size() != 19 && ts != ts.substr(0, 19) + "+08" &&
        ts != ts.substr(0, 19) + "+08:00")
        return false;
    if (ts.size() < 19 || ts[4] != '-' || ts[7] != '-' || ts[10] != ' ' ||
        ts[13] != ':' || ts[16] != ':')
        return false;
    int parts[6]{};
    const int start[] = {0,5,8,11,14,17}, length[] = {4,2,2,2,2,2};
    for (int p = 0; p < 6; ++p)
        for (int i = 0; i < length[p]; ++i)
        {
            char c = ts[start[p] + i];
            if (c < '0' || c > '9') return false;
            parts[p] = parts[p] * 10 + c - '0';
        }
    date = parts[0] * 10000 + parts[1] * 100 + parts[2];
    int time = parts[3] * 10000 + parts[4] * 100 + parts[5];
    if (!valid_date(date) || !valid_time(time)) return false;
    ts = format_ts(date, job.minute ? time : job.default_time);
    return true;
}

bool valid_values(const CsvRow &row)
{
    for (float value : row.values)
        if (!std::isfinite(value)) return false;
    return row.values[4] >= 0 && row.values[5] >= 0 &&
           row.values[1] >= row.values[2] &&
           row.values[1] >= row.values[0] && row.values[1] >= row.values[3] &&
           row.values[2] <= row.values[0] && row.values[2] <= row.values[3];
}

bool parse_csv_row(const std::string &line, size_t column_count, CsvRow &row, const ExportJob &job)
{
    if(column_count<9||column_count>10)return false;
    std::array<std::string_view,10> columns;const std::string_view view(line);std::size_t start=0,count=0;
    for(;;){
        if(count==column_count)return false;
        const auto end=view.find(',',start);
        columns[count++]=trim_view(view.substr(start,end==std::string_view::npos?end:end-start));
        if(end==std::string_view::npos)break;
        start=end+1;
    }
    if(count!=column_count||columns[0]!=job.symbol||columns[1]!=job.period||columns[2].empty())return false;
    row.symbol=job.symbol;row.period=job.period;row.ts.assign(columns[2].data(),columns[2].size());
    if (!parse_timestamp(row.ts, row.date, job))
        return false;
    for (int i = 0; i < 6; ++i)
        if (!parse_number(columns[i + 3], row.values[i])) return false;
    row.key = row_key(row.period, row.date, row.ts);
    return valid_values(row);
}

bool read_line(std::ifstream &input, std::string &line)
{
    char buffer[2048];
    if (!input.getline(buffer, sizeof(buffer))) return false;
    size_t size = static_cast<size_t>(input.gcount() - (input.eof() ? 0 : 1));
    line.assign(buffer, size);
    if (line.find('\0') != std::string::npos)
        throw std::runtime_error("CSV 含有 NUL 字节，不能截断后继续读取");
    line = trim(line);
    return true;
}

bool load_existing(const std::string &path, std::map<std::string, CsvRow> &rows, const ExportJob &job)
{
    std::ifstream input;
    input.exceptions(std::ios::badbit);
    input.open(path, std::ios::binary);
    if (!input.is_open()) return false;
    std::string header;
    if (!read_line(input, header)) return false;
    if (header.compare(0, 3, "\xEF\xBB\xBF") == 0) header.erase(0, 3);
    std::string extra = key_column(job.period);
    size_t columns = 9;
    if (header != kHeader)
    {
        if (extra.empty() || header != std::string(kHeader) + "," + extra) return false;
        columns = 10;
    }
    std::string line;
    std::string previous;
    while (read_line(input, line))
    {
        if (line.empty()) continue;
        CsvRow row;
        if (!parse_csv_row(line, columns, row, job)) return false;
        if (!previous.empty() && row.ts < previous) return false;
        previous = row.ts;
        auto old = rows.find(row.key);
        if (old != rows.end() && row.ts == old->second.ts &&
            !std::equal(row.values, row.values + 6, old->second.values))
            return false;
        // 旧文件可能有同周期多个快照，迁移时保留日期较晚者。
        if (old == rows.end() || row.ts >= old->second.ts)
            rows[row.key] = row;
    }
    return input.eof() && !input.bad() && !rows.empty();
}

bool save_reject(const char *message)
{
    LOG_ERROR("%s", message);
    return false;
}

bool write_csv(ExportJob &s)
{
    SaveTiming timing(s.save_ms);
    if (s.count <= 0 || s.count > kMaxCount || s.dates.size() != static_cast<size_t>(s.count) ||
        (s.minute && s.times.size() != static_cast<size_t>(s.count)) || !valid_time(s.default_time))
        return save_reject("后台任务条数、日期时间数组长度或默认时间无效");
    for (const auto &values : s.series)
        if (values.size() != static_cast<size_t>(s.count))
            return save_reject("后台数值数组长度不一致");
    std::map<std::string, CsvRow> incoming;
    int previous_date = 0, previous_time = 0;
    for (int i = 0; i < s.count; ++i)
    {
        int raw = 0, time = s.default_time;
        // 先核验有限性、整数性和范围，再转整数；合法年月日才可计算周月年键。
        if (!integer(s.dates[i], 0, 80991231, raw) || !valid_date(raw + 19000000))
            return save_reject("日期不是有效的通达信 DATE");
        if (s.minute)
        {
            int hhmm = 0;
            if (!integer(s.times[i], 0, 2359, hhmm) || hhmm % 100 >= 60)
                return save_reject("分钟周期的 TIME 必须为合法 HHMM");
            time = hhmm * 100;
        }
        const int date = raw + 19000000;
        if (i > 0 && (date < previous_date || (date == previous_date && time <= previous_time)))
            return save_reject("日期时间必须严格升序，不能重复");
        previous_date = date;
        previous_time = time;
        CsvRow row;
        row.symbol = s.symbol;
        row.period = s.period;
        row.date = date;
        const int order[] = {0,2,3,1,4,5};
        for (int v = 0; v < 6; ++v) row.values[v] = s.series[order[v]][i];
        if (!valid_values(row))
            return save_reject("数值无法安全格式化、存在负量额或价格关系矛盾");
        row.ts = format_ts(date, time);
        row.key = row_key(row.period, row.date, row.ts);
        incoming[row.key] = row;
    }
    timing.next();
    if (!ensure_dir(s.csv_dir))
        return save_reject("无法创建输出目录");
    std::string path = join_path(s.csv_dir, s.symbol + "." + s.period + ".csv");
    FileHandle lock(CreateFileA((path + ".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE)
        return save_reject("目标正在导出，或无法创建写锁，请稍后重跑");
    std::map<std::string, CsvRow> rows;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes))
    {
        if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            attributes.nFileSizeHigh || attributes.nFileSizeLow > kMaxCsvBytes ||
            !load_existing(path, rows, s))
            return save_reject("旧 CSV 无法完整读取或内容不合法，已停止覆盖");
    }
    else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        return save_reject("无法检查旧 CSV，已停止覆盖");
    timing.next();
    for (const auto &entry : incoming)
    {
        auto old = rows.find(entry.first);
        if (old != rows.end() && entry.second.ts < old->second.ts)
            return save_reject("输入快照早于同周期已保存记录，已停止回退覆盖");
        rows[entry.first] = entry.second;
    }
    std::vector<const CsvRow *> ordered;
    ordered.reserve(rows.size());
    for (const auto &entry : rows) ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(),
              [](const CsvRow *a, const CsvRow *b) { return a->ts < b->ts; });
    timing.next();
    TempFile temp;
    if (!GetTempFileNameA(s.csv_dir.c_str(), "tdx", 0, temp.path))
    {
        temp.path[0] = '\0';
        return save_reject("无法创建同目录临时文件");
    }
    std::ofstream output;
    output.exceptions(std::ios::badbit);
    output.open(temp.path, std::ios::binary | std::ios::trunc);
    output.imbue(std::locale::classic());
    if (!output.is_open()) return save_reject("无法打开临时文件");
    output << kHeader;
    std::string extra = key_column(s.period);
    if (!extra.empty()) output << ',' << extra;
    output << '\n';
    std::string line;
    line.reserve(192);
    for (const CsvRow *row : ordered)
    {
        line.clear();line.append(row->symbol).push_back(',');line.append(row->period).push_back(',');line.append(row->ts);
        // 同原流的double转换和六位有效数字，复用行缓冲，避免逐值locale/流构造开销。
        for(float value:row->values){
            char text[64];const auto formatted=std::to_chars(text,text+sizeof(text),static_cast<double>(value),std::chars_format::general,6);
            if(formatted.ec!=std::errc{})return save_reject("数值格式化缓冲不足，已保留原文件");
            line.push_back(',');line.append(text,static_cast<std::size_t>(formatted.ptr-text));
        }
        if(!extra.empty()){line.push_back(',');line.append(row->key);}
        line.push_back('\n');output.write(line.data(),static_cast<std::streamsize>(line.size()));
    }
    output.flush();
    bool written = output.good();
    output.close();
    if (!written || output.fail()) return save_reject("临时文件写入或关闭失败");
    {
        FileHandle file(CreateFileA(temp.path, GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
            return save_reject("无法打开临时文件进行大小检查和刷盘");
        LARGE_INTEGER bytes{};
        if (!GetFileSizeEx(file.value, &bytes) || bytes.QuadPart <= 0 || bytes.QuadPart > kMaxCsvBytes)
            return save_reject("临时 CSV 大小检查失败或超过 512 MiB，已保留原文件");
        if (!FlushFileBuffers(file.value))
            return save_reject("临时文件刷盘失败");
    }
    if (!MoveFileExA(temp.path, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DWORD error = GetLastError();
        LOG_ERROR("替换 CSV 失败，已保留原文件，Windows 错误码 %lu", error);
        return false;
    }
    temp.path[0] = '\0';
    try
    {
        LOG_INFO("保存成功：%s，本次 %d 条，合并后 %u 条",
                 path.c_str(), s.count, static_cast<unsigned>(rows.size()));
    }
    catch (...) {} // 提交后的日志失败不能把已成功的保存报告成失败。
    return true;
}

bool perform_save(ExportJob &job) noexcept
{
    const auto begin = ExportClock::now();
    job.queue_wait_ms = std::chrono::duration<double, std::milli>(begin - job.queued).count();
    bool saved = false;
    try
    {
        auto next_report = begin;
        std::uint64_t retries = 0;
        for (;;)
        {
            try
            {
                std::fill(job.save_ms, job.save_ms + 4, 0.0);
                saved = write_csv(job);
                if (retries) try { LOG_INFO("内存分配恢复 phase=save id=%llu retries=%llu waited_ms=%.3f", job.id, retries, elapsed_ms(begin)); } catch (...) {}
                break;
            }
            catch (const std::bad_alloc &)
            {
                ++retries;
                const auto now = ExportClock::now();
                if (now >= next_report)
                {
                    next_report = now + std::chrono::seconds(5);
                    try { LOG_WARN("内存分配重试 phase=save id=%llu retries=%llu retry_ms=200 waited_ms=%.3f", job.id, retries, elapsed_ms(begin)); } catch (...) {}
                }
                Sleep(200);
            }
        }
    }
    catch (const std::exception &error)
    {
        try { LOG_ERROR("保存失败 id=%llu symbol=%s period=%s 原因=%s",
                        job.id, job.symbol.c_str(), job.period.c_str(), error.what()); } catch (...) {}
    }
    catch (...)
    {
        try { LOG_ERROR("保存异常 id=%llu symbol=%s period=%s，请重新导出",
                        job.id, job.symbol.c_str(), job.period.c_str()); } catch (...) {}
    }
    job.save_call_ms = elapsed_ms(begin);
    log_export_timing(job, saved ? 1.0f : -1.0f);
    if (!saved)
        try { LOG_ERROR("后台任务失败 id=%llu symbol=%s period=%s，原文件未提交，本任务需重导",
                        job.id, job.symbol.c_str(), job.period.c_str()); } catch (...) {}
    return saved;
}
}
