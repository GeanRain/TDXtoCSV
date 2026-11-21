/****************************************************************************
 * SaveAsCSV TDX Export DLL
 ****************************************************************************/
#pragma once

#ifndef __LINKTDX_H__
#define __LINKTDX_H__

#include <windows.h>
#include "FxIndicator.h"
 

#define MATHLIBRARY_EXPORTS
#ifdef MATHLIBRARY_EXPORTS
#define MATHLIBRARY_API __declspec(dllexport)
#else
#define MATHLIBRARY_API __declspec(dllimport)
#endif

// 1 提交后台(3,1,0)，2 日期时间(DATE,TIME,上一步结果)，3 成对数据(类型,数组1,数组2)。
// 4 开始(代码,市场,PERIOD)，5 确认(上一步结果,TQFLAG,0)；成功 1，失败 -1，详见 DOC/tdx_mapping.md。
// 3 不回填返回数组：1开收、2高低、3量额；4除权、5除息各传SPLITBARS/SPLIT。
// 后台按计数0提取事件，除权原样保存、除息乘10；错误记录于本次状态和日志。
// 2 的 1 仅表示原始日期已独立复制，逐条行情校验由后台执行；最终结果查看日志、6/7 号。
// 6 查询(项目0..6,0,0)，7 排空停机(0,0,0)，8 停止接收(0,0,0)。提交成功不等于CSV已保存。
extern "C" MATHLIBRARY_API void Func1(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func2(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func3(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func4(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func5(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func6(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func7(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);
extern "C" MATHLIBRARY_API void Func8(int nCount, float *pOut, float *pIn1, float *pIn2, float *pIn3);

#endif
