# SaveAsCSV - 通达信数据导出 DLL

将通达信（TDX）传入的行情数据导出为标准化 CSV 文件，供后续数据库导入与分析使用。

## 程序作用

- 接收日期、时间、开高低收、成交量和成交额，统一格式化并输出 CSV。
- 自动识别全量/增量：无同名文件时新建，已有文件时合并并更新数据。
- 支持 1、5、15、30、60 分钟，以及日、周、月、年周期，当前仅接受不复权数据。
- DLL 接收数据，由独立的 32 位后台程序完成校验、合并和保存；支持多线程处理不同文件。
- 输出路径、队列容量、保存线程数及日志可通过配置文件控制。

## 环境与依赖

- Windows 10/11、32 位通达信。
- 发布版 DLL 和后台程序已包含 C++ 运行库，无需另装 Visual C++ 运行库或 .NET。
- 编译需要 Visual Studio 2022 的 C++ v143 工具链及 Windows SDK。

## 目录结构（关键部分）

- `SaveAsCSV/link/tdx/linkTDX.cpp`：通达信接口与数据接收。
- `SaveAsCSV/export/process_writer.*`：后台启动及管道通信。
- `SaveAsCSV/export/async_writer.*`：队列与保存线程。
- `SaveAsCSVWorker/main.cpp`：独立后台程序入口。
- `shared/csv_store.*`：CSV 校验、合并与保存。
- `SaveAsCSV/utils`：日志与 INI 配置读取。
- `DOC`：详细用法、接口约定与设计说明。

## 安装与编译

1. 使用 Visual Studio 打开 `Stock_Indicator.sln`。
2. 选择 `Release / x86`，编译 DLL 和后台程序两个工程。
3. 将 `Release/SaveAsCSV.dll` 与 `Release/SaveAsCSVWorker.exe` 配套复制到通达信插件目录，默认放在同一目录。
4. 在通达信公式管理器中绑定 DLL，记下绑定编号；首次加载会自动生成 `saveAsCVS.ini`，注册接口时自动启动并连接后台。

已有编译文件时，直接从第 3 步开始；更新程序前先停止扫描并完成下方的排空操作。

## 配置文件

`saveAsCVS.ini` 位于 DLL 同目录，默认配置如下：

```ini
[log]
mode=file
level=1
file=saveAsCSV.log

[export]
csv_dir=
symbol_width=6
default_time=150000

[async]
queue_capacity=1000
queue_memory_mb=2048
worker_threads=8
worker_path=SaveAsCSVWorker.exe
```

- `csv_dir`：为空时输出到 DLL 同目录的 `csv` 文件夹；相对路径以 DLL 目录为基准。
- `symbol_width`：代码补零宽度，范围 1—12，默认 6。
- `default_time`：日、周、月、年的统一时间，格式为 HHMMSS，默认 150000；分钟周期使用通达信传入的时间。
- `queue_capacity`：采集预留及未完成任务的数量上限，默认 1000，包含正在保存的任务。
- `queue_memory_mb`：后台任务数据额度，单位 MiB，默认 2048；不包含旧 CSV 解析和合并等临时内存。
- `worker_threads`：同时保存的线程数，范围 1—128，默认 8；同一个文件按提交顺序处理。
- `worker_path`：后台程序相对于 DLL 的路径，默认同目录；也可写 `backend\SaveAsCSVWorker.exe`，不接受绝对路径。
- `log.mode`：`file` 写文件、`debugview` 调试输出、`console` 控制台或 `disable` 关闭日志。
- `log.level`：0 全部，1 信息及以上，2 警告及错误，3 仅错误；`log.file` 设置日志文件名。

`export` 配置每次开始导出时读取；修改 `async` 配置后先排空停机，再开始导出。修改日志配置后重新启动通达信。后台程序路径改变后，配置、日志及 CSV 相对路径仍以 DLL 目录为基准。

旧配置中的 `append`、`symbol_suffix`、`tz` 不再使用；CSV 自动合并，时间不添加时区后缀。

## 使用方式

### 调用顺序（通达信侧）

1. 绑定 DLL，新建条件选股公式，设置**使用数据为全部**，提前下载所需历史数据并使用不复权数据。
2. 复制下方完整公式。示例按第 4 号绑定编写；如绑定到其他编号，统一替换 `TDXDLL4` 中的 `4`。
3. 选择支持的周期和股票范围，执行条件选股。每个品种按顺序执行完整公式。

```text
START:=TDXDLL4(4,STR2CON(CODE),SETCODE,PERIOD); {重置数据并设置代码、市场和周期}
META:=TDXDLL4(5,START,TQFLAG,0); {检查不复权状态和支持的周期}
DT:=TDXDLL4(2,DATE,TIME,META); {复制原始日期时间，后台校验}
P1:=TDXDLL4(3,1,OPEN,DT); {复制开盘价，后台校验}
P2:=TDXDLL4(3,2,CLOSE,P1); {复制收盘价，后台校验}
P3:=TDXDLL4(3,3,HIGH,P2); {复制最高价，后台校验}
P4:=TDXDLL4(3,4,LOW,P3); {复制最低价，后台校验}
P5:=TDXDLL4(3,5,VOL,P4); {复制成交量，按通达信原值保存}
P6:=TDXDLL4(3,6,AMOUNT,P5); {复制成交额，按通达信原值保存}
RESULT:=TDXDLL4(1,3,P6,0); {将完整数据提交后台保存}
F:RESULT=1; {仅表示提交成功，CSV完成情况须看后台结果}
```

各步骤通过上一步的返回值建立调用依赖，不要改回旧版重置公式或固定的 `F:1`。周标识由日期自动计算，无需传入 `WEEKOFYEAR`。

### 扫描结束后确认保存完成

停止选股扫描后，单独对一只股票运行以下排空公式一次，等待后台处理完已提交任务：

```text
ENDRESULT:=TDXDLL4(7,0,0,0); {停止接收，等待保存完成并停止后台}
F:ENDRESULT=1; {本轮无接收、提交及保存错误才为真}
```

不要把排空公式加到每只股票的导出公式末尾。返回真表示本轮成功完成；返回假时查看 DLL 目录的日志，修正问题后重新导出失败品种。

通达信退出、DLL 卸载或重新绑定时，旧后台立即退出，不等待队列，未完成任务会丢失。需要保留全部任务时，先排空再退出或解绑；入库和更新程序前也应完成排空。重新绑定会启动新后台，排空后再次运行导出公式也会自动启动后台。卸载或重绑前先停止选股，确保 DLL 接口已返回。

### 输出文件

- 默认目录：DLL 同目录的 `csv` 文件夹，可通过 `csv_dir` 修改。
- 文件名：`代码.市场.周期.csv`，例如 `600000.SH.1d.csv`。
- 分钟周期标识为 `1m`、`5m`、`15m`、`30m`、`60m`；日、周、月、年分别为 `1d`、`1w`、`1mo`、`1y`。
- 基础表头：`symbol,period,ts,open,high,low,close,volume,amount`。
- 周、月、年分别追加 `week_key`、`month_key`、`year_key`，按对应周期标识合并；其他周期按 `ts` 合并，最终按时间升序保存。

重复导出合并同一个文件，保留历史、更新已有日期并补入新日期，不按导出次数生成副本。合并结果与原文件完全一致时跳过 CSV 写盘；有变化时先写临时文件，再替换目标文件。数据或旧 CSV 校验失败时保留原文件。

### 查看后台状态

CSV 目录中的状态文件名如 `_bg_12-1000_8-8`，表示等待 12 个任务、队列上限 1000、正在保存 8 个、线程上限 8。有任务时每 5 秒更新，全部完成后归零并暂停刷新。

状态归零不代表没有保存错误，仍需检查排空结果和日志。状态查询、停止接收等管理公式见 [调用说明](DOC/tdx_calling.md)。

## 文档

- [详细配置与使用说明](DOC/使用说明.md)
- [通达信调用顺序、管理公式与周期规则](DOC/tdx_calling.md)
- [输入输出约定、CSV 合并与文件保护](DOC/tdx_mapping.md)
- [异步保存设计说明](DOC/多线程异步保存设计方案.md)
