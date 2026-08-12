/******************************************************************************
 * Shader 指令统计分析器 — 核心逻辑（无 Qt UI 依赖）
 *
 * 支持两种源码：
 *   HLSL → D3DCompile → DXBC
 *   GLSL → glslangValidator → SPIR-V
 *
 * 均支持优化级别选择，产出反汇编文本与按类别的 opcode 统计。
 ******************************************************************************/

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QMap>

// 源码语言
enum class ShaderSourceLang
{
  HLSL,   // → DXBC
  GLSL,   // → SPIR-V
};

// 编译优化级别
enum class HLSLOptLevel
{
  O0 = 0,   // 禁用优化
  O1 = 1,
  O2 = 2,
  O3 = 3,   // 最大优化
};

// 指令分类（HLSL/GLSL 共用一套语义，具体分类算法各自实现）
enum class OpcodeCategory
{
  ALU,
  Memory,
  Control,
  Atomic,
  Derivative,
  Convert,
  Bitwise,
  ExtInst,   // SPIR-V 专用：GLSL.std.450 扩展指令（sin/cos/normalize 等）
  Other,
  Decl,
};

struct HLSLAnalyzeResult
{
  bool compileOk = false;
  int instructionSlots = -1;           // DXBC 才有：fxc 报告的槽数
  int totalOpcodes = 0;                // 可执行 opcode 总数（不含 decl）
  QMap<OpcodeCategory, int> categoryCounts;
  QVector<QPair<QString, int>> topOpcodes;
  QString disassembly;
  QString errors;
  ShaderSourceLang lang = ShaderSourceLang::HLSL;
};

// GLSL / SPIR-V target 环境
enum class GLSLTargetEnv
{
  GLES_3_1,    // 默认，你的手机场景
  GLES_3_0,
  GLES_3_2,
  Vulkan_1_0,
  Vulkan_1_1,
  Vulkan_1_2,
  Vulkan_1_3,
  OpenGL_4_5,
};

// GLSL shader stage（枚举与 D3D 的 profile 分开）
enum class GLSLStage
{
  Vertex,
  Fragment,
  Compute,
  Geometry,
  TessControl,
  TessEvaluation,
};

// —— HLSL / DXBC 分析（原有）——
HLSLAnalyzeResult AnalyzeHLSL(const QString &source,
                              const QString &profile,
                              const QString &entry,
                              const QStringList &defines,
                              HLSLOptLevel opt);

// —— GLSL / SPIR-V 分析（新增）——
HLSLAnalyzeResult AnalyzeGLSL(const QString &source,
                              GLSLStage stage,
                              GLSLTargetEnv env,
                              const QString &entry,
                              const QStringList &defines,
                              HLSLOptLevel opt);

// 查询 opcode 的中文说明（自动识别 HLSL/DXBC 或 SPIR-V opcode）
QString LookupOpcodeHelp(const QString &opcode);

// 查询分类的中文名 + 说明
QString CategoryLabel(OpcodeCategory cat);

// 定位 glslangValidator.exe（返回空字符串表示未找到）
QString LocateGlslangValidator();
void SetGlslangValidatorPath(const QString &p);

