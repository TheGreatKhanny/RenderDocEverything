/******************************************************************************
 * HLSL DXBC 指令统计分析器 — 核心逻辑（无 Qt 依赖）
 *
 * 功能：
 *   1. 用 D3DCompile 编译 HLSL（支持 /O0~/O3 优化）
 *   2. 用 D3DDisassemble 反汇编为文本
 *   3. 解析汇编文本，按 opcode 分类统计
 *
 * 通过运行时 LoadLibrary 加载 d3dcompiler_47.dll，避免链接依赖。
 ******************************************************************************/

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QMap>

// 编译优化级别
enum class HLSLOptLevel
{
  O0 = 0,   // /Od 禁用优化
  O1 = 1,   // 默认
  O2 = 2,
  O3 = 3,   // 最大优化
};

// 指令分类
enum class OpcodeCategory
{
  ALU,       // 算术逻辑
  Memory,    // 内存 / 纹理
  Control,   // 控制流
  Atomic,    // 原子操作
  Derivative,// 导数
  Convert,   // 类型转换
  Bitwise,   // 位运算
  Other,     // 未分类
  Decl,      // 声明（不计入运行时）
};

struct HLSLAnalyzeResult
{
  bool compileOk = false;
  int instructionSlots = -1;           // fxc 报告的槽数
  int totalOpcodes = 0;                // 本工具统计的可执行 opcode 总数（不含 decl）
  QMap<OpcodeCategory, int> categoryCounts;
  QVector<QPair<QString, int>> topOpcodes;   // 按次数降序
  QString disassembly;
  QString errors;
};

// 主入口：编译 + 反汇编 + 分析
HLSLAnalyzeResult AnalyzeHLSL(const QString &source,
                              const QString &profile,
                              const QString &entry,
                              const QStringList &defines,
                              HLSLOptLevel opt);

// 查询 opcode 的中文说明（含 _sat / _indexable 等后缀识别）
QString LookupOpcodeHelp(const QString &opcode);

// 查询分类的中文名 + 说明
QString CategoryLabel(OpcodeCategory cat);
