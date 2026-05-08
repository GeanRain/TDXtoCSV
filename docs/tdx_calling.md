# 通达信 DLL 调用顺序

本文定义通达信侧调用 DLL 的推荐顺序与职责，用于稳定导出 CSV。

## 推荐调用顺序（每个 symbol + period）

1. `Func1(cmd=0)` 重置内部状态
2. `Func1(cmd=1)` 设置代码与市场类型
3. `Func1(cmd=2)` 设置周期
4. `Func2` 传入日期/时间/周序号数组
5. `Func3` 按类型传入 OHLC + 成交量 + 成交额（共 6 次）
6. `Func1(cmd=3)` 写 CSV

## 通达信公式示例（TDXDLL1）

### 1) Reset

```
TDXDLL1(1,0,0,0);
```

### 2) Set Symbol + Market

```
TDXDLL1(1,1,STR2CON(CODE),SETCODE);
```

- `CODE` 为通达信代码
- `SETCODE` 为市场类型码（见下表）

### 3) Set Period

```
TDXDLL1(1,2,PERIOD,0);
```

### 4) Set Date/Time/Week

```
TDXDLL1(2,DATE,TIME,WEEKOFYEAR);
```

- `pIn3=WEEKOFYEAR` 主要用于周线 `week_key`
- `pIn3` 建议使用序列输入（与 `DATE/TIME` 同长度），不要传单个常量值
- 若周线未提供有效 `pIn3`（`<=0`），DLL 会回退按日期计算 ISO week

### 5) Set OHLC/Volume/Amount by Type

```
TDXDLL1(3,1,OPEN,0);
TDXDLL1(3,2,CLOSE,0);
TDXDLL1(3,3,HIGH,0);
TDXDLL1(3,4,LOW,0);
TDXDLL1(3,5,VOL,0);
TDXDLL1(3,6,AMOUNT,0);
```

### 6) Write CSV

```
TDXDLL1(1,3,0,0);
```

## 市场类型码映射

- `0` -> `SZ`
- `1` -> `SH`
- `2` -> `BJ`
- `47` -> `CFFEX`
- `28` -> `CZCE`
- `29` -> `DCE`
- `30` -> `SHFE`
- `27` -> `HKI`
- `31` -> `HK`
- `48` -> `HKGEM`

## 周期码映射（使用部分）

- `0` -> `1m`
- `1` -> `5m`
- `3` -> `30m`
- `5` -> `1d`
- `6` -> `1w`
- `7` -> `1mo`
- `10` -> `1q`
- `11` -> `1y`

## 注意事项

- 所有输入数组类型为 `float`，长度由 `nCount` 决定。
- `VOL` 单位为“手”，`AMOUNT` 单位为“元”。
- CSV 输出路径与格式由 `saveAsCVS.ini` 的 `[export]` 控制。
- CSV 表头按周期区分：
  - 周线（`1w`）：`symbol,period,ts,open,high,low,close,volume,amount,week_key`
  - 月线（`1mo`）：`symbol,period,ts,open,high,low,close,volume,amount,month_key`
  - 季线（`1q`）：`symbol,period,ts,open,high,low,close,volume,amount,quarter_key`
  - 年线（`1y`）：`symbol,period,ts,open,high,low,close,volume,amount,year_key`
  - 分钟线/日线/其他周期：`symbol,period,ts,open,high,low,close,volume,amount`
- 导出模式自动识别：无同名文件按全量写入；有同名文件按周期对应唯一键增量合并。
- 合并唯一键按周期区分：周线用 `week_key`，月线用 `month_key`，季线用 `quarter_key`，年线用 `year_key`，其他周期用 `ts`。
- 合并时若同唯一键数据不同，则覆盖旧数据；最终文件按 `ts` 升序写回。
- 旧版周/月/季/年 CSV（无 key 列）可直接兼容，下一次导出时会自动按新规则收敛。
- 输出文件名格式：`代码.市场.周期.csv`，如 `600000.SH.1d.csv`。
- 缺少必要字段时会跳过写入并记录日志。
