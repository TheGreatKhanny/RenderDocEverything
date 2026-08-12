/******************************************************************************
 * HLSL DXBC 指令统计分析器 — 核心实现
 ******************************************************************************/

#include "HLSLAnalyzer.h"

#include <QRegularExpression>
#include <QLibrary>
#include <QFileInfo>
#include <QCoreApplication>
#include <QSet>

#include <windows.h>
#include <d3dcompiler.h>

// D3DCompile 函数签名（避免直接链接 d3dcompiler.lib）
typedef HRESULT(WINAPI *pD3DCompile)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                     const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                     LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                                     ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

typedef HRESULT(WINAPI *pD3DDisassemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, UINT Flags,
                                         LPCSTR szComments, ID3DBlob **ppDisassembly);

// —— d3dcompiler_47.dll 单次加载 ——
static HMODULE g_d3dcompiler = nullptr;
static pD3DCompile     g_pD3DCompile = nullptr;
static pD3DDisassemble g_pD3DDisassemble = nullptr;

static bool EnsureD3DCompiler(QString &err)
{
  if(g_pD3DCompile && g_pD3DDisassemble)
    return true;

  // 依次尝试 47 / 46 / 43，都是 RenderDoc 常用的版本
  const wchar_t *dllNames[] = {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"};
  for(const wchar_t *name : dllNames)
  {
    g_d3dcompiler = LoadLibraryW(name);
    if(g_d3dcompiler)
      break;
  }
  if(!g_d3dcompiler)
  {
    err = QObject::tr("无法加载 d3dcompiler_47.dll。请确认已安装 Windows 10 SDK 或 D3D 运行时。");
    return false;
  }

  g_pD3DCompile =
      (pD3DCompile)GetProcAddress(g_d3dcompiler, "D3DCompile");
  g_pD3DDisassemble =
      (pD3DDisassemble)GetProcAddress(g_d3dcompiler, "D3DDisassemble");

  if(!g_pD3DCompile || !g_pD3DDisassemble)
  {
    err = QObject::tr("d3dcompiler 缺少必要的导出函数（D3DCompile / D3DDisassemble）。");
    return false;
  }
  return true;
}

// —— opcode 分类 ——
static OpcodeCategory ClassifyOpcode(const QString &opLower)
{
  // 控制流
  static const QStringList ctrl = {
      QStringLiteral("if"),        QStringLiteral("else"),
      QStringLiteral("endif"),     QStringLiteral("loop"),
      QStringLiteral("endloop"),   QStringLiteral("break"),
      QStringLiteral("breakc"),    QStringLiteral("continue"),
      QStringLiteral("continuec"), QStringLiteral("switch"),
      QStringLiteral("case"),      QStringLiteral("default"),
      QStringLiteral("endswitch"), QStringLiteral("ret"),
      QStringLiteral("retc"),      QStringLiteral("call"),
      QStringLiteral("callc"),     QStringLiteral("label"),
      QStringLiteral("discard"),   QStringLiteral("nop"),
  };
  if(ctrl.contains(opLower))
    return OpcodeCategory::Control;
  if(opLower.startsWith(QStringLiteral("discard")))
    return OpcodeCategory::Control;

  // 内存 / 采样 / 存储
  static const QStringList memPrefix = {
      QStringLiteral("sample"),  QStringLiteral("ld"),
      QStringLiteral("gather"),  QStringLiteral("store"),
      QStringLiteral("bufinfo"), QStringLiteral("resinfo"),
      QStringLiteral("lod"),     QStringLiteral("check_access"),
  };
  for(const QString &pfx : memPrefix)
  {
    if(opLower == pfx || opLower.startsWith(pfx + QStringLiteral("_")))
      return OpcodeCategory::Memory;
  }

  // 原子操作
  if(opLower.startsWith(QStringLiteral("atomic_")) ||
     opLower.startsWith(QStringLiteral("imm_atomic_")))
    return OpcodeCategory::Atomic;

  // 导数
  if(opLower.startsWith(QStringLiteral("deriv_")))
    return OpcodeCategory::Derivative;

  // 类型转换
  static const QStringList conv = {
      QStringLiteral("ftoi"), QStringLiteral("ftou"),
      QStringLiteral("itof"), QStringLiteral("utof"),
      QStringLiteral("f16tof32"), QStringLiteral("f32tof16"),
      QStringLiteral("dtof"), QStringLiteral("ftod"),
  };
  if(conv.contains(opLower))
    return OpcodeCategory::Convert;

  // 位运算
  static const QStringList bitOps = {
      QStringLiteral("and"),      QStringLiteral("or"),
      QStringLiteral("xor"),      QStringLiteral("not"),
      QStringLiteral("ishl"),     QStringLiteral("ishr"),
      QStringLiteral("ushr"),     QStringLiteral("bfi"),
      QStringLiteral("bfrev"),    QStringLiteral("ubfe"),
      QStringLiteral("ibfe"),     QStringLiteral("countbits"),
      QStringLiteral("firstbit_hi"), QStringLiteral("firstbit_lo"),
      QStringLiteral("firstbit_shi"),
  };
  if(bitOps.contains(opLower))
    return OpcodeCategory::Bitwise;

  // ALU：涵盖大部分浮点/整数算术
  static const QStringList alu = {
      QStringLiteral("mov"),  QStringLiteral("movc"), QStringLiteral("mad"),
      QStringLiteral("mul"),  QStringLiteral("add"),  QStringLiteral("sub"),
      QStringLiteral("div"),  QStringLiteral("min"),  QStringLiteral("max"),
      QStringLiteral("frc"),  QStringLiteral("rsq"),  QStringLiteral("rcp"),
      QStringLiteral("sqrt"), QStringLiteral("exp"),  QStringLiteral("log"),
      QStringLiteral("sincos"), QStringLiteral("sin"), QStringLiteral("cos"),
      QStringLiteral("tan"),
      QStringLiteral("dp2"), QStringLiteral("dp3"),  QStringLiteral("dp4"),
      QStringLiteral("dp2add"),
      QStringLiteral("round_ne"), QStringLiteral("round_ni"),
      QStringLiteral("round_pi"), QStringLiteral("round_z"),
      QStringLiteral("iadd"), QStringLiteral("isub"), QStringLiteral("imul"),
      QStringLiteral("imad"), QStringLiteral("imin"), QStringLiteral("imax"),
      QStringLiteral("idiv"), QStringLiteral("udiv"), QStringLiteral("umin"),
      QStringLiteral("umax"), QStringLiteral("umad"), QStringLiteral("umul"),
      QStringLiteral("ineg"),
      QStringLiteral("eq"),  QStringLiteral("ne"),  QStringLiteral("lt"),
      QStringLiteral("ge"),  QStringLiteral("ieq"), QStringLiteral("ine"),
      QStringLiteral("ilt"), QStringLiteral("ige"), QStringLiteral("ult"),
      QStringLiteral("uge"),
      QStringLiteral("abs"), QStringLiteral("neg"), QStringLiteral("saturate"),
  };
  if(alu.contains(opLower))
    return OpcodeCategory::ALU;

  return OpcodeCategory::Other;
}

// —— 分析反汇编文本 ——
static void ParseDisassembly(const QString &asmText, HLSLAnalyzeResult &out)
{
  // 提取 fxc 报告的 instruction slots
  QRegularExpression slotsRe(
      QStringLiteral("Approximately\\s+(\\d+)\\s+instruction slots used"),
      QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatch m = slotsRe.match(asmText);
  if(m.hasMatch())
    out.instructionSlots = m.captured(1).toInt();

  // 首 token 提取正则；兼容可能存在的前导 "N:" 行号
  QRegularExpression opRe(QStringLiteral("^\\s*(?:\\d+:\\s*)?([a-z][a-z0-9_]*)"),
                          QRegularExpression::CaseInsensitiveOption);

  QMap<QString, int> opCounts;
  const QStringList lines = asmText.split(QLatin1Char('\n'));
  for(const QString &rawLine : lines)
  {
    QString line = rawLine.trimmed();
    if(line.isEmpty())
      continue;
    if(line.startsWith(QStringLiteral("//")))
      continue;

    QRegularExpressionMatch mm = opRe.match(line);
    if(!mm.hasMatch())
      continue;
    QString op = mm.captured(1).toLower();

    // 忽略声明与 target profile 行
    if(op.startsWith(QStringLiteral("dcl_")) ||
       op == QStringLiteral("ps_5_0") || op == QStringLiteral("vs_5_0") ||
       op == QStringLiteral("cs_5_0") || op == QStringLiteral("gs_5_0") ||
       op == QStringLiteral("hs_5_0") || op == QStringLiteral("ds_5_0") ||
       op == QStringLiteral("ps_5_1") || op == QStringLiteral("vs_5_1") ||
       op == QStringLiteral("cs_5_1") ||
       op == QStringLiteral("ps_4_0") || op == QStringLiteral("vs_4_0") ||
       op == QStringLiteral("cs_4_0"))
    {
      out.categoryCounts[OpcodeCategory::Decl] += 1;
      continue;
    }

    // 分类时剥离已知后缀，让 mul_sat / dp3_sat 归到 mul / dp3
    QString base = op;
    QStringList suffixes = {QStringLiteral("_sat"), QStringLiteral("_indexable"),
                            QStringLiteral("_aoffimmi")};
    bool stripped = true;
    while(stripped)
    {
      stripped = false;
      for(const QString &sfx : suffixes)
      {
        if(base.endsWith(sfx))
        {
          base.chop(sfx.length());
          stripped = true;
          break;
        }
      }
    }

    OpcodeCategory cat = ClassifyOpcode(base);
    out.categoryCounts[cat] += 1;
    if(cat != OpcodeCategory::Decl)
      out.totalOpcodes += 1;

    opCounts[op] += 1;
  }

  // 生成 Top opcodes（按次数降序）
  QVector<QPair<QString, int>> pairs;
  pairs.reserve(opCounts.size());
  for(auto it = opCounts.constBegin(); it != opCounts.constEnd(); ++it)
    pairs.push_back(qMakePair(it.key(), it.value()));

  std::sort(pairs.begin(), pairs.end(),
            [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
              if(a.second != b.second)
                return a.second > b.second;
              return a.first < b.first;
            });

  const int TOP_N = 30;
  if(pairs.size() > TOP_N)
    pairs.resize(TOP_N);
  out.topOpcodes = pairs;
}

// —— 主入口 ——
HLSLAnalyzeResult AnalyzeHLSL(const QString &source, const QString &profile, const QString &entry,
                              const QStringList &defines, HLSLOptLevel opt)
{
  HLSLAnalyzeResult result;

  QString loadErr;
  if(!EnsureD3DCompiler(loadErr))
  {
    result.errors = loadErr;
    return result;
  }

  // 优化标志
  UINT flags = 0;
  switch(opt)
  {
    case HLSLOptLevel::O0: flags |= D3DCOMPILE_SKIP_OPTIMIZATION; break;
    case HLSLOptLevel::O1: flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1; break;
    case HLSLOptLevel::O2: flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2; break;
    case HLSLOptLevel::O3: flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3; break;
  }

  // 组装 defines（宏名/值必须持有到调用结束）
  QVector<QByteArray> defineStrings;
  defineStrings.reserve(defines.size() * 2);
  QVector<D3D_SHADER_MACRO> macros;
  for(const QString &d : defines)
  {
    QString kv = d.trimmed();
    if(kv.isEmpty())
      continue;
    int eq = kv.indexOf(QLatin1Char('='));
    QString name = (eq >= 0) ? kv.left(eq) : kv;
    QString val  = (eq >= 0) ? kv.mid(eq + 1) : QStringLiteral("1");
    defineStrings.push_back(name.toUtf8());
    defineStrings.push_back(val.toUtf8());
    D3D_SHADER_MACRO m;
    m.Name       = defineStrings[defineStrings.size() - 2].constData();
    m.Definition = defineStrings[defineStrings.size() - 1].constData();
    macros.push_back(m);
  }
  D3D_SHADER_MACRO terminator = {nullptr, nullptr};
  macros.push_back(terminator);

  const QByteArray srcUtf8    = source.toUtf8();
  const QByteArray entryUtf8  = entry.toUtf8();
  const QByteArray targetUtf8 = profile.toUtf8();

  ID3DBlob *code = nullptr;
  ID3DBlob *errs = nullptr;
  HRESULT hr = g_pD3DCompile(srcUtf8.constData(), (SIZE_T)srcUtf8.size(),
                             "shader.hlsl",
                             macros.constData(),
                             D3D_COMPILE_STANDARD_FILE_INCLUDE,
                             entryUtf8.constData(),
                             targetUtf8.constData(),
                             flags, 0, &code, &errs);

  if(errs)
  {
    result.errors = QString::fromUtf8(
        (const char *)errs->GetBufferPointer(), (int)errs->GetBufferSize());
    errs->Release();
  }

  if(FAILED(hr) || !code)
  {
    if(result.errors.isEmpty())
      result.errors = QObject::tr("D3DCompile 失败，HRESULT=0x%1")
                          .arg((quint32)hr, 8, 16, QLatin1Char('0'));
    return result;
  }

  ID3DBlob *asmBlob = nullptr;
  hr = g_pD3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(),
                         0, nullptr, &asmBlob);
  code->Release();

  if(FAILED(hr) || !asmBlob)
  {
    result.errors += QObject::tr("\nD3DDisassemble 失败，HRESULT=0x%1")
                         .arg((quint32)hr, 8, 16, QLatin1Char('0'));
    return result;
  }

  result.disassembly = QString::fromUtf8(
      (const char *)asmBlob->GetBufferPointer(), (int)asmBlob->GetBufferSize());
  asmBlob->Release();

  ParseDisassembly(result.disassembly, result);
  result.compileOk = true;
  return result;
}

// ================== 分类中文标签 ==================
QString CategoryLabel(OpcodeCategory cat)
{
  switch(cat)
  {
    case OpcodeCategory::ALU:
      return QObject::tr("ALU（算术逻辑：加减乘除、点积、rsq/exp/log 等，着色器主体计算）");
    case OpcodeCategory::Memory:
      return QObject::tr("Memory（内存访问：纹理采样 sample、缓冲区读写 ld/store，延迟通常最高）");
    case OpcodeCategory::Control:
      return QObject::tr("Control（控制流：if/else/loop/ret/discard，可能引起波前发散）");
    case OpcodeCategory::Atomic:
      return QObject::tr("Atomic（原子操作：atomic_* / imm_atomic_*，跨线程同步）");
    case OpcodeCategory::Derivative:
      return QObject::tr("Derivative（导数：ddx/ddy 及粗/细版本，像素着色器专用）");
    case OpcodeCategory::Convert:
      return QObject::tr("Convert（类型转换：ftoi/itof/f16tof32 等）");
    case OpcodeCategory::Bitwise:
      return QObject::tr("Bitwise（位运算：and/or/xor/shl/shr/bit* 等）");
    case OpcodeCategory::ExtInst:
      return QObject::tr("ExtInst（GLSL.std.450 扩展指令：sin/cos/normalize/dot 等）");
    case OpcodeCategory::Other:
      return QObject::tr("Other（未分类：不常见或自定义指令）");
    case OpcodeCategory::Decl:
      return QObject::tr("Decl（声明：dcl_* 输入输出/资源声明，不计入运行时开销）");
  }
  return QString();
}

// ================== opcode 中文说明表 ==================
static QMap<QString, QString> BuildOpcodeHelp()
{
  QMap<QString, QString> m;
  auto add = [&](const char *k, const QString &v) { m[QString::fromLatin1(k)] = v; };

  // 浮点算术
  add("mov",    QObject::tr("寄存器搬运"));
  add("movc",   QObject::tr("条件搬运（三元选择 c?a:b）"));
  add("mad",    QObject::tr("乘加融合 a*b+c"));
  add("mul",    QObject::tr("浮点乘法"));
  add("add",    QObject::tr("浮点加法"));
  add("sub",    QObject::tr("浮点减法"));
  add("div",    QObject::tr("浮点除法"));
  add("min",    QObject::tr("取较小值"));
  add("max",    QObject::tr("取较大值"));
  add("frc",    QObject::tr("取小数部分 x - floor(x)"));
  add("round_ne",QObject::tr("就近取整（Round to Nearest Even）"));
  add("round_ni",QObject::tr("向下取整（floor）"));
  add("round_pi",QObject::tr("向上取整（ceil）"));
  add("round_z", QObject::tr("向零取整（trunc）"));
  // 点积
  add("dp2",    QObject::tr("二维点积"));
  add("dp3",    QObject::tr("三维点积"));
  add("dp4",    QObject::tr("四维点积"));
  add("dp2add", QObject::tr("二维点积再加"));
  // 数学函数
  add("rsq",    QObject::tr("平方根倒数 1/sqrt(x)"));
  add("rcp",    QObject::tr("倒数 1/x"));
  add("sqrt",   QObject::tr("平方根"));
  add("exp",    QObject::tr("指数 2^x"));
  add("log",    QObject::tr("对数 log2(x)"));
  add("sincos", QObject::tr("正余弦（占 2 slot）"));
  add("sin",    QObject::tr("正弦"));
  add("cos",    QObject::tr("余弦"));
  add("tan",    QObject::tr("正切"));
  // 类型转换
  add("ftoi",   QObject::tr("float → int（截断）"));
  add("ftou",   QObject::tr("float → uint（截断）"));
  add("itof",   QObject::tr("int → float"));
  add("utof",   QObject::tr("uint → float"));
  add("f16tof32",QObject::tr("half → float"));
  add("f32tof16",QObject::tr("float → half"));
  // 整数算术
  add("iadd",   QObject::tr("整数加法"));
  add("isub",   QObject::tr("整数减法"));
  add("imul",   QObject::tr("整数乘法（32×32→64，取高低）"));
  add("imad",   QObject::tr("整数乘加"));
  add("imin",   QObject::tr("有符号整数取小"));
  add("imax",   QObject::tr("有符号整数取大"));
  add("idiv",   QObject::tr("有符号整数除法（同时得商与余数）"));
  add("udiv",   QObject::tr("无符号整数除法"));
  add("umin",   QObject::tr("无符号整数取小"));
  add("umax",   QObject::tr("无符号整数取大"));
  add("umad",   QObject::tr("无符号整数乘加"));
  add("umul",   QObject::tr("无符号整数乘法"));
  add("ineg",   QObject::tr("整数取负"));
  // 位运算
  add("and",    QObject::tr("按位与"));
  add("or",     QObject::tr("按位或"));
  add("xor",    QObject::tr("按位异或"));
  add("not",    QObject::tr("按位取反"));
  add("ishl",   QObject::tr("算术左移"));
  add("ishr",   QObject::tr("算术右移（有符号）"));
  add("ushr",   QObject::tr("逻辑右移（无符号）"));
  add("bfi",    QObject::tr("位域插入"));
  add("bfrev",  QObject::tr("位反转"));
  add("ubfe",   QObject::tr("无符号位域提取"));
  add("ibfe",   QObject::tr("有符号位域提取"));
  add("countbits",   QObject::tr("位计数（popcount）"));
  add("firstbit_hi", QObject::tr("最高位为 1 的位置"));
  add("firstbit_lo", QObject::tr("最低位为 1 的位置"));
  add("firstbit_shi",QObject::tr("最高有效符号位位置"));
  // 比较
  add("eq",     QObject::tr("浮点等于比较"));
  add("ne",     QObject::tr("浮点不等比较"));
  add("lt",     QObject::tr("浮点小于比较"));
  add("ge",     QObject::tr("浮点大于等于比较"));
  add("ieq",    QObject::tr("整数等于比较"));
  add("ine",    QObject::tr("整数不等比较"));
  add("ilt",    QObject::tr("有符号整数小于比较"));
  add("ige",    QObject::tr("有符号整数大于等于比较"));
  add("ult",    QObject::tr("无符号整数小于比较"));
  add("uge",    QObject::tr("无符号整数大于等于比较"));
  // 导数
  add("deriv_rtx",       QObject::tr("屏幕 X 方向导数（ddx）"));
  add("deriv_rty",       QObject::tr("屏幕 Y 方向导数（ddy）"));
  add("deriv_rtx_coarse",QObject::tr("ddx 粗略版（2x2 quad）"));
  add("deriv_rty_coarse",QObject::tr("ddy 粗略版（2x2 quad）"));
  add("deriv_rtx_fine",  QObject::tr("ddx 精细版"));
  add("deriv_rty_fine",  QObject::tr("ddy 精细版"));
  // 纹理采样
  add("sample",       QObject::tr("纹理采样"));
  add("sample_l",     QObject::tr("指定 LOD 采样"));
  add("sample_b",     QObject::tr("带 LOD 偏移的采样"));
  add("sample_d",     QObject::tr("指定梯度的采样（各向异性）"));
  add("sample_c",     QObject::tr("比较采样（阴影 PCF）"));
  add("sample_c_lz",  QObject::tr("比较采样 LOD=0（点阴影）"));
  add("gather4",      QObject::tr("四点采样（返回 2x2 邻域）"));
  add("gather4_c",    QObject::tr("四点比较采样"));
  add("gather4_po",   QObject::tr("四点采样（可编程偏移）"));
  add("gather4_po_c", QObject::tr("四点比较采样（可编程偏移）"));
  add("ld",           QObject::tr("无采样器读取（Load，按整数坐标）"));
  add("ld_ms",        QObject::tr("MSAA 纹理读取"));
  add("ld_raw",       QObject::tr("ByteAddressBuffer 原始读取"));
  add("ld_structured",QObject::tr("StructuredBuffer 读取"));
  add("ld_uav_typed", QObject::tr("从类型化 UAV 读取"));
  add("lod",          QObject::tr("查询 LOD 计算结果"));
  add("resinfo",      QObject::tr("查询资源尺寸 / mip 数"));
  add("bufinfo",      QObject::tr("查询 buffer 元素数量"));
  add("check_access_fully_mapped", QObject::tr("稀疏资源访问检查"));
  // 存储
  add("store_raw",        QObject::tr("ByteAddressBuffer 写入"));
  add("store_structured", QObject::tr("StructuredBuffer 写入"));
  add("store_uav_typed",  QObject::tr("类型化 UAV 写入"));
  // 原子
  add("atomic_and",       QObject::tr("原子按位与"));
  add("atomic_or",        QObject::tr("原子按位或"));
  add("atomic_xor",       QObject::tr("原子按位异或"));
  add("atomic_iadd",      QObject::tr("原子整数加"));
  add("atomic_imax",      QObject::tr("原子有符号取大"));
  add("atomic_imin",      QObject::tr("原子有符号取小"));
  add("atomic_umax",      QObject::tr("原子无符号取大"));
  add("atomic_umin",      QObject::tr("原子无符号取小"));
  add("atomic_cmp_store", QObject::tr("原子比较写入"));
  add("imm_atomic_alloc", QObject::tr("追加缓冲区分配位置"));
  add("imm_atomic_consume", QObject::tr("消费缓冲区取位置"));
  add("imm_atomic_iadd",  QObject::tr("即时原子加（返回旧值）"));
  add("imm_atomic_and",   QObject::tr("即时原子与"));
  add("imm_atomic_or",    QObject::tr("即时原子或"));
  add("imm_atomic_xor",   QObject::tr("即时原子异或"));
  add("imm_atomic_exch",  QObject::tr("即时原子交换"));
  add("imm_atomic_cmp_exch", QObject::tr("即时原子比较交换"));
  // 控制流
  add("if",        QObject::tr("分支开始（if 条件成立进入）"));
  add("else",      QObject::tr("分支 else 分支"));
  add("endif",     QObject::tr("分支结束"));
  add("loop",      QObject::tr("循环开始"));
  add("endloop",   QObject::tr("循环结束"));
  add("break",     QObject::tr("跳出循环"));
  add("breakc",    QObject::tr("条件跳出循环"));
  add("continue",  QObject::tr("继续下一轮循环"));
  add("continuec", QObject::tr("条件 continue"));
  add("switch",    QObject::tr("switch 开始"));
  add("case",      QObject::tr("case 分支"));
  add("default",   QObject::tr("default 分支"));
  add("endswitch", QObject::tr("switch 结束"));
  add("ret",       QObject::tr("函数返回"));
  add("retc",      QObject::tr("条件返回"));
  add("call",      QObject::tr("函数调用"));
  add("callc",     QObject::tr("条件函数调用"));
  add("label",     QObject::tr("标签"));
  add("discard",   QObject::tr("丢弃像素（无条件）"));
  add("discard_nz",QObject::tr("条件丢弃像素（非零则丢弃）"));
  add("discard_z", QObject::tr("条件丢弃像素（为零则丢弃）"));
  add("nop",       QObject::tr("空操作"));
  // 同步 / GS / HS
  add("sync",         QObject::tr("线程组同步屏障"));
  add("emit",         QObject::tr("GS 发射顶点"));
  add("cut",          QObject::tr("GS 结束条带"));
  add("emit_stream",  QObject::tr("GS 指定流发射"));
  add("cut_stream",   QObject::tr("GS 指定流结束"));
  add("emitthencut",  QObject::tr("GS 发射并结束"));
  add("hs_control_point_phase", QObject::tr("HS 控制点阶段"));
  add("hs_fork_phase",          QObject::tr("HS fork 阶段"));
  add("hs_join_phase",          QObject::tr("HS join 阶段"));
  // 特殊
  add("abs",      QObject::tr("取绝对值（源修饰符）"));
  add("neg",      QObject::tr("取负（源修饰符）"));
  add("saturate", QObject::tr("饱和到 [0,1]（目标修饰符）"));

  // ================== SPIR-V opcode 中文表 ==================
  // 命名与 SPIR-V 规范一致（大小写敏感，Op 开头）
  // 参考：https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html

  // ---- 浮点算术 ----
  add("OpFAdd",   QObject::tr("浮点加法"));
  add("OpFSub",   QObject::tr("浮点减法"));
  add("OpFMul",   QObject::tr("浮点乘法"));
  add("OpFDiv",   QObject::tr("浮点除法"));
  add("OpFMod",   QObject::tr("浮点取模"));
  add("OpFRem",   QObject::tr("浮点求余"));
  add("OpFNegate",QObject::tr("浮点取负"));
  add("OpDot",    QObject::tr("向量点积"));
  add("OpVectorTimesScalar", QObject::tr("向量×标量"));
  add("OpMatrixTimesScalar", QObject::tr("矩阵×标量"));
  add("OpVectorTimesMatrix", QObject::tr("向量×矩阵"));
  add("OpMatrixTimesVector", QObject::tr("矩阵×向量"));
  add("OpMatrixTimesMatrix", QObject::tr("矩阵×矩阵"));
  add("OpOuterProduct", QObject::tr("外积"));
  add("OpTranspose",    QObject::tr("矩阵转置"));
  // ---- 整数算术 ----
  add("OpIAdd",     QObject::tr("整数加法"));
  add("OpISub",     QObject::tr("整数减法"));
  add("OpIMul",     QObject::tr("整数乘法"));
  add("OpSDiv",     QObject::tr("有符号整数除法"));
  add("OpUDiv",     QObject::tr("无符号整数除法"));
  add("OpSRem",     QObject::tr("有符号整数求余"));
  add("OpSMod",     QObject::tr("有符号整数取模"));
  add("OpUMod",     QObject::tr("无符号整数取模"));
  add("OpSNegate",  QObject::tr("整数取负"));
  // ---- 位运算 ----
  add("OpBitwiseAnd", QObject::tr("按位与"));
  add("OpBitwiseOr",  QObject::tr("按位或"));
  add("OpBitwiseXor", QObject::tr("按位异或"));
  add("OpNot",        QObject::tr("按位取反"));
  add("OpShiftLeftLogical",     QObject::tr("逻辑左移"));
  add("OpShiftRightLogical",    QObject::tr("逻辑右移"));
  add("OpShiftRightArithmetic", QObject::tr("算术右移"));
  add("OpBitFieldInsert",   QObject::tr("位域插入"));
  add("OpBitFieldSExtract", QObject::tr("有符号位域提取"));
  add("OpBitFieldUExtract", QObject::tr("无符号位域提取"));
  add("OpBitReverse", QObject::tr("位反转"));
  add("OpBitCount",   QObject::tr("位计数（popcount）"));
  // ---- 比较 / 逻辑 ----
  add("OpFOrdEqual",         QObject::tr("浮点有序等于"));
  add("OpFOrdNotEqual",      QObject::tr("浮点有序不等"));
  add("OpFOrdLessThan",      QObject::tr("浮点有序小于"));
  add("OpFOrdGreaterThan",   QObject::tr("浮点有序大于"));
  add("OpFOrdLessThanEqual", QObject::tr("浮点有序小于等于"));
  add("OpFOrdGreaterThanEqual", QObject::tr("浮点有序大于等于"));
  add("OpFUnordEqual",       QObject::tr("浮点无序等于"));
  add("OpFUnordNotEqual",    QObject::tr("浮点无序不等"));
  add("OpFUnordLessThan",    QObject::tr("浮点无序小于"));
  add("OpFUnordGreaterThan", QObject::tr("浮点无序大于"));
  add("OpFUnordLessThanEqual",   QObject::tr("浮点无序小于等于"));
  add("OpFUnordGreaterThanEqual",QObject::tr("浮点无序大于等于"));
  add("OpIEqual",            QObject::tr("整数等于"));
  add("OpINotEqual",         QObject::tr("整数不等"));
  add("OpSLessThan",         QObject::tr("有符号整数小于"));
  add("OpSGreaterThan",      QObject::tr("有符号整数大于"));
  add("OpSLessThanEqual",    QObject::tr("有符号整数小于等于"));
  add("OpSGreaterThanEqual", QObject::tr("有符号整数大于等于"));
  add("OpULessThan",         QObject::tr("无符号整数小于"));
  add("OpUGreaterThan",      QObject::tr("无符号整数大于"));
  add("OpULessThanEqual",    QObject::tr("无符号整数小于等于"));
  add("OpUGreaterThanEqual", QObject::tr("无符号整数大于等于"));
  add("OpLogicalAnd",        QObject::tr("逻辑与"));
  add("OpLogicalOr",         QObject::tr("逻辑或"));
  add("OpLogicalNot",        QObject::tr("逻辑非"));
  add("OpLogicalEqual",      QObject::tr("逻辑等于"));
  add("OpLogicalNotEqual",   QObject::tr("逻辑不等"));
  add("OpSelect",            QObject::tr("三元选择 c?a:b"));
  add("OpAny",               QObject::tr("向量任一为真"));
  add("OpAll",               QObject::tr("向量全为真"));
  add("OpIsNan",             QObject::tr("判 NaN"));
  add("OpIsInf",             QObject::tr("判 Inf"));
  // ---- 类型转换 ----
  add("OpConvertFToU",  QObject::tr("float → uint"));
  add("OpConvertFToS",  QObject::tr("float → int"));
  add("OpConvertSToF",  QObject::tr("int → float"));
  add("OpConvertUToF",  QObject::tr("uint → float"));
  add("OpFConvert",     QObject::tr("浮点精度转换（如 highp↔mediump）"));
  add("OpUConvert",     QObject::tr("无符号整数位宽转换"));
  add("OpSConvert",     QObject::tr("有符号整数位宽转换"));
  add("OpBitcast",      QObject::tr("按位重解释（reinterpret_cast）"));
  add("OpQuantizeToF16",QObject::tr("量化到 half"));
  // ---- 内存 / 加载 / 存储 ----
  add("OpLoad",         QObject::tr("从内存加载"));
  add("OpStore",        QObject::tr("写入内存"));
  add("OpAccessChain",  QObject::tr("生成成员/元素访问指针"));
  add("OpInBoundsAccessChain", QObject::tr("生成成员/元素访问指针（越界未定义）"));
  add("OpPtrAccessChain",      QObject::tr("生成指针偏移"));
  add("OpArrayLength",  QObject::tr("查询 SSBO 运行时数组长度"));
  add("OpCopyMemory",   QObject::tr("内存拷贝"));
  add("OpVariable",     QObject::tr("变量声明"));
  // ---- 纹理采样 ----
  add("OpImageSampleImplicitLod", QObject::tr("纹理采样（自动 LOD，等价 texture()）"));
  add("OpImageSampleExplicitLod", QObject::tr("纹理采样（指定 LOD，textureLod）"));
  add("OpImageSampleDrefImplicitLod", QObject::tr("阴影比较采样（自动 LOD）"));
  add("OpImageSampleDrefExplicitLod", QObject::tr("阴影比较采样（指定 LOD）"));
  add("OpImageSampleProjImplicitLod", QObject::tr("投影采样（自动 LOD）"));
  add("OpImageSampleProjExplicitLod", QObject::tr("投影采样（指定 LOD）"));
  add("OpImageSampleProjDrefImplicitLod", QObject::tr("投影阴影采样"));
  add("OpImageSampleProjDrefExplicitLod", QObject::tr("投影阴影采样（指定 LOD）"));
  add("OpImageFetch",   QObject::tr("按整数坐标取纹素（texelFetch）"));
  add("OpImageGather",  QObject::tr("四点采样（textureGather）"));
  add("OpImageDrefGather", QObject::tr("四点阴影采样"));
  add("OpImageRead",    QObject::tr("从 image 读（imageLoad）"));
  add("OpImageWrite",   QObject::tr("向 image 写（imageStore）"));
  add("OpImage",        QObject::tr("从 sampledImage 取出 image"));
  add("OpImageQuerySize",QObject::tr("查询纹理尺寸（textureSize）"));
  add("OpImageQuerySizeLod", QObject::tr("查询指定 LOD 尺寸"));
  add("OpImageQueryLod",     QObject::tr("查询采样 LOD"));
  add("OpImageQueryLevels",  QObject::tr("查询 mip 级数"));
  add("OpImageQuerySamples", QObject::tr("查询 MSAA 采样数"));
  add("OpSampledImage", QObject::tr("绑定 sampler 与 image"));
  // ---- 导数 ----
  add("OpDPdx",         QObject::tr("屏幕 X 方向导数（ddx / dFdx）"));
  add("OpDPdy",         QObject::tr("屏幕 Y 方向导数（ddy / dFdy）"));
  add("OpDPdxCoarse",   QObject::tr("ddx 粗略版（2x2 quad）"));
  add("OpDPdyCoarse",   QObject::tr("ddy 粗略版（2x2 quad）"));
  add("OpDPdxFine",     QObject::tr("ddx 精细版"));
  add("OpDPdyFine",     QObject::tr("ddy 精细版"));
  add("OpFwidth",       QObject::tr("|ddx|+|ddy|"));
  add("OpFwidthCoarse", QObject::tr("fwidth 粗略版"));
  add("OpFwidthFine",   QObject::tr("fwidth 精细版"));
  // ---- 控制流 ----
  add("OpLabel",        QObject::tr("基本块标签"));
  add("OpBranch",       QObject::tr("无条件跳转"));
  add("OpBranchConditional", QObject::tr("条件跳转"));
  add("OpSwitch",       QObject::tr("switch 分派"));
  add("OpLoopMerge",    QObject::tr("循环合并点声明"));
  add("OpSelectionMerge",QObject::tr("选择合并点声明（if/switch）"));
  add("OpReturn",       QObject::tr("函数返回"));
  add("OpReturnValue",  QObject::tr("返回带值"));
  add("OpKill",         QObject::tr("丢弃像素（discard）"));
  add("OpTerminateInvocation", QObject::tr("终止本次调用"));
  add("OpUnreachable",  QObject::tr("不可达标记"));
  add("OpFunctionCall", QObject::tr("函数调用"));
  add("OpPhi",          QObject::tr("SSA phi 节点"));
  // ---- 原子 / 同步 ----
  add("OpAtomicIAdd",   QObject::tr("原子整数加"));
  add("OpAtomicISub",   QObject::tr("原子整数减"));
  add("OpAtomicIIncrement", QObject::tr("原子自增"));
  add("OpAtomicIDecrement", QObject::tr("原子自减"));
  add("OpAtomicAnd",    QObject::tr("原子与"));
  add("OpAtomicOr",     QObject::tr("原子或"));
  add("OpAtomicXor",    QObject::tr("原子异或"));
  add("OpAtomicSMin",   QObject::tr("原子有符号取小"));
  add("OpAtomicSMax",   QObject::tr("原子有符号取大"));
  add("OpAtomicUMin",   QObject::tr("原子无符号取小"));
  add("OpAtomicUMax",   QObject::tr("原子无符号取大"));
  add("OpAtomicExchange",         QObject::tr("原子交换"));
  add("OpAtomicCompareExchange",  QObject::tr("原子比较交换"));
  add("OpAtomicLoad",   QObject::tr("原子加载"));
  add("OpAtomicStore",  QObject::tr("原子存储"));
  add("OpControlBarrier",  QObject::tr("控制屏障（同步）"));
  add("OpMemoryBarrier",   QObject::tr("内存屏障"));
  // ---- 复合 / 构造 ----
  add("OpCompositeConstruct", QObject::tr("构造向量/矩阵/结构体"));
  add("OpCompositeExtract",   QObject::tr("从复合类型提取元素"));
  add("OpCompositeInsert",    QObject::tr("向复合类型插入元素"));
  add("OpVectorExtractDynamic", QObject::tr("动态索引取分量"));
  add("OpVectorInsertDynamic",  QObject::tr("动态索引写分量"));
  add("OpVectorShuffle",      QObject::tr("向量分量重排（swizzle）"));
  add("OpCopyObject",         QObject::tr("对象拷贝"));
  add("OpUndef",              QObject::tr("未定义值"));
  add("OpConstant",           QObject::tr("常量声明"));
  add("OpConstantComposite",  QObject::tr("常量复合类型"));
  add("OpConstantTrue",       QObject::tr("常量 true"));
  add("OpConstantFalse",      QObject::tr("常量 false"));
  add("OpConstantNull",       QObject::tr("零值常量"));
  add("OpSpecConstant",       QObject::tr("特化常量"));
  add("OpSpecConstantComposite", QObject::tr("特化常量复合"));
  add("OpSpecConstantOp",     QObject::tr("特化常量运算"));
  // ---- 声明 / 元数据 ----
  add("OpFunction",           QObject::tr("函数开始"));
  add("OpFunctionEnd",        QObject::tr("函数结束"));
  add("OpFunctionParameter",  QObject::tr("函数参数"));
  add("OpCapability",         QObject::tr("能力声明"));
  add("OpExtension",          QObject::tr("扩展声明"));
  add("OpExtInstImport",      QObject::tr("导入扩展指令集（如 GLSL.std.450）"));
  add("OpMemoryModel",        QObject::tr("内存模型声明"));
  add("OpEntryPoint",         QObject::tr("入口点声明"));
  add("OpExecutionMode",      QObject::tr("执行模式声明"));
  add("OpTypeVoid",           QObject::tr("类型：void"));
  add("OpTypeBool",           QObject::tr("类型：bool"));
  add("OpTypeInt",            QObject::tr("类型：int/uint"));
  add("OpTypeFloat",          QObject::tr("类型：float"));
  add("OpTypeVector",         QObject::tr("类型：向量"));
  add("OpTypeMatrix",         QObject::tr("类型：矩阵"));
  add("OpTypeImage",          QObject::tr("类型：图像"));
  add("OpTypeSampler",        QObject::tr("类型：采样器"));
  add("OpTypeSampledImage",   QObject::tr("类型：sampledImage"));
  add("OpTypeArray",          QObject::tr("类型：数组"));
  add("OpTypeRuntimeArray",   QObject::tr("类型：运行时数组"));
  add("OpTypeStruct",         QObject::tr("类型：结构体"));
  add("OpTypePointer",        QObject::tr("类型：指针"));
  add("OpTypeFunction",       QObject::tr("类型：函数"));
  add("OpDecorate",           QObject::tr("装饰（如 layout/location）"));
  add("OpMemberDecorate",     QObject::tr("成员装饰"));
  add("OpName",               QObject::tr("变量名调试信息"));
  add("OpMemberName",         QObject::tr("成员名调试信息"));
  add("OpSource",             QObject::tr("源码语言声明"));
  add("OpSourceExtension",    QObject::tr("源码扩展"));
  add("OpString",             QObject::tr("字符串常量"));
  add("OpLine",               QObject::tr("行号调试信息"));
  add("OpNoLine",             QObject::tr("清除行号"));

  add("OpExtInst",            QObject::tr("扩展指令调用（下面细分具体函数）"));

  // ---- GLSL.std.450 扩展指令（用 "Ext.<Name>" 作 key） ----
  add("Ext.Round",       QObject::tr("就近取整"));
  add("Ext.RoundEven",   QObject::tr("就近取偶取整"));
  add("Ext.Trunc",       QObject::tr("向零取整"));
  add("Ext.FAbs",        QObject::tr("浮点绝对值"));
  add("Ext.SAbs",        QObject::tr("整数绝对值"));
  add("Ext.FSign",       QObject::tr("浮点符号"));
  add("Ext.SSign",       QObject::tr("整数符号"));
  add("Ext.Floor",       QObject::tr("向下取整"));
  add("Ext.Ceil",        QObject::tr("向上取整"));
  add("Ext.Fract",       QObject::tr("取小数部分"));
  add("Ext.Radians",     QObject::tr("角度→弧度"));
  add("Ext.Degrees",     QObject::tr("弧度→角度"));
  add("Ext.Sin",         QObject::tr("正弦"));
  add("Ext.Cos",         QObject::tr("余弦"));
  add("Ext.Tan",         QObject::tr("正切"));
  add("Ext.Asin",        QObject::tr("反正弦"));
  add("Ext.Acos",        QObject::tr("反余弦"));
  add("Ext.Atan",        QObject::tr("反正切"));
  add("Ext.Sinh",        QObject::tr("双曲正弦"));
  add("Ext.Cosh",        QObject::tr("双曲余弦"));
  add("Ext.Tanh",        QObject::tr("双曲正切"));
  add("Ext.Asinh",       QObject::tr("反双曲正弦"));
  add("Ext.Acosh",       QObject::tr("反双曲余弦"));
  add("Ext.Atanh",       QObject::tr("反双曲正切"));
  add("Ext.Atan2",       QObject::tr("双参反正切"));
  add("Ext.Pow",         QObject::tr("幂 pow(x,y)"));
  add("Ext.Exp",         QObject::tr("指数 e^x"));
  add("Ext.Log",         QObject::tr("自然对数"));
  add("Ext.Exp2",        QObject::tr("指数 2^x"));
  add("Ext.Log2",        QObject::tr("对数 log2"));
  add("Ext.Sqrt",        QObject::tr("平方根"));
  add("Ext.InverseSqrt", QObject::tr("平方根倒数 1/sqrt"));
  add("Ext.Determinant", QObject::tr("行列式"));
  add("Ext.MatrixInverse", QObject::tr("矩阵求逆"));
  add("Ext.Modf",        QObject::tr("拆分整数与小数部分"));
  add("Ext.FMin",        QObject::tr("浮点取小"));
  add("Ext.FMax",        QObject::tr("浮点取大"));
  add("Ext.UMin",        QObject::tr("无符号整数取小"));
  add("Ext.SMin",        QObject::tr("有符号整数取小"));
  add("Ext.UMax",        QObject::tr("无符号整数取大"));
  add("Ext.SMax",        QObject::tr("有符号整数取大"));
  add("Ext.FClamp",      QObject::tr("浮点 clamp"));
  add("Ext.UClamp",      QObject::tr("无符号 clamp"));
  add("Ext.SClamp",      QObject::tr("有符号 clamp"));
  add("Ext.FMix",        QObject::tr("线性插值 mix / lerp"));
  add("Ext.Step",        QObject::tr("阶跃函数"));
  add("Ext.SmoothStep",  QObject::tr("平滑插值 smoothstep"));
  add("Ext.Fma",         QObject::tr("融合乘加 a*b+c"));
  add("Ext.Length",      QObject::tr("向量长度"));
  add("Ext.Distance",    QObject::tr("两点距离"));
  add("Ext.Cross",       QObject::tr("叉积"));
  add("Ext.Normalize",   QObject::tr("向量归一化"));
  add("Ext.FaceForward", QObject::tr("面向法线校正"));
  add("Ext.Reflect",     QObject::tr("反射向量"));
  add("Ext.Refract",     QObject::tr("折射向量"));
  add("Ext.PackHalf2x16",  QObject::tr("打包 2 个 half 到 uint"));
  add("Ext.UnpackHalf2x16",QObject::tr("从 uint 解包 2 个 half"));
  add("Ext.PackSnorm2x16", QObject::tr("打包 2 个 snorm 到 uint"));
  add("Ext.UnpackSnorm2x16", QObject::tr("解包 snorm2x16"));
  add("Ext.PackUnorm2x16", QObject::tr("打包 2 个 unorm 到 uint"));
  add("Ext.UnpackUnorm2x16", QObject::tr("解包 unorm2x16"));
  add("Ext.PackSnorm4x8",  QObject::tr("打包 4 个 snorm 到 uint"));
  add("Ext.UnpackSnorm4x8",QObject::tr("解包 snorm4x8"));
  add("Ext.PackUnorm4x8",  QObject::tr("打包 4 个 unorm 到 uint"));
  add("Ext.UnpackUnorm4x8",QObject::tr("解包 unorm4x8"));
  add("Ext.FindILsb",    QObject::tr("最低有效位位置"));
  add("Ext.FindUMsb",    QObject::tr("最高有效位位置（无符号）"));
  add("Ext.FindSMsb",    QObject::tr("最高有效位位置（有符号）"));

  return m;
}

QString LookupOpcodeHelp(const QString &opcode)
{
  static const QMap<QString, QString> table = BuildOpcodeHelp();

  // SPIR-V opcode 都是 Op 开头且大小写敏感（例如 OpFAdd / OpImageSampleImplicitLod）
  // GLSL.std.450 扩展指令按 "Ext.<Name>" 存
  if(opcode.startsWith(QStringLiteral("Op")) || opcode.startsWith(QStringLiteral("Ext.")))
  {
    auto it = table.find(opcode);
    if(it != table.end())
      return it.value();
    if(opcode.startsWith(QStringLiteral("Ext.")))
      return QObject::tr("GLSL.std.450 扩展指令，请查阅规范");
    return QObject::tr("SPIR-V 指令，请查阅规范");
  }

  // 后缀说明
  auto suffixDesc = [](const QString &sfx) -> QString {
    if(sfx == QStringLiteral("_sat"))       return QObject::tr("结果饱和到 [0,1]");
    if(sfx == QStringLiteral("_indexable")) return QObject::tr("可索引资源版本");
    if(sfx == QStringLiteral("_aoffimmi"))  return QObject::tr("带立即数偏移");
    return QString();
  };

  QString op = opcode.toLower();

  // 1. 精确匹配
  auto it = table.find(op);
  if(it != table.end())
    return it.value();

  // 2. 逐段剥离后缀查表
  QString base = op;
  QStringList collectedDesc;
  const QStringList knownSfx = {QStringLiteral("_sat"), QStringLiteral("_indexable"),
                                QStringLiteral("_aoffimmi")};
  bool stripped = true;
  while(stripped)
  {
    stripped = false;
    for(const QString &sfx : knownSfx)
    {
      if(base.endsWith(sfx))
      {
        collectedDesc.push_front(suffixDesc(sfx));
        base.chop(sfx.length());
        stripped = true;
        auto it2 = table.find(base);
        if(it2 != table.end())
          return it2.value() + QStringLiteral("（") + collectedDesc.join(QStringLiteral("，")) +
                 QStringLiteral("）");
        break;
      }
    }
  }

  // 3. 前缀兜底：例如 sample_indexable_something
  for(auto it3 = table.constBegin(); it3 != table.constEnd(); ++it3)
  {
    if(op.startsWith(it3.key() + QStringLiteral("_")))
      return it3.value() + QObject::tr("（变体）");
  }

  return QObject::tr("未收录指令，请查阅规范文档");
}

// ============================================================================
// ================ GLSL / SPIR-V 分析（调 glslangValidator） =================
// ============================================================================

#include <QProcess>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>

static QString g_GlslangPath;

void SetGlslangValidatorPath(const QString &p)
{
  g_GlslangPath = p;
}

QString LocateGlslangValidator()
{
  if(!g_GlslangPath.isEmpty() && QFileInfo::exists(g_GlslangPath))
    return g_GlslangPath;

  // 常见搜索位置
  QStringList candidates;

  // 1. RenderDoc 正式安装目录 plugins
  candidates << QStringLiteral("C:/Program Files/RenderDoc/plugins/spirv/glslangValidator.exe")
             << QStringLiteral("C:/Program Files (x86)/RenderDoc/plugins/spirv/glslangValidator.exe");

  // 2. 当前 qrenderdoc 目录下的 plugins/spirv
  QString exeDir = QCoreApplication::applicationDirPath();
  candidates << exeDir + QStringLiteral("/plugins/spirv/glslangValidator.exe")
             << exeDir + QStringLiteral("/glslangValidator.exe");

  // 3. Vulkan SDK
  QByteArray vk = qgetenv("VULKAN_SDK");
  if(!vk.isEmpty())
    candidates << QString::fromLocal8Bit(vk) + QStringLiteral("/Bin/glslangValidator.exe");

  // 4. PATH
  QString onPath = QStandardPaths::findExecutable(QStringLiteral("glslangValidator"));
  if(!onPath.isEmpty())
    candidates << onPath;

  for(const QString &c : candidates)
  {
    if(QFileInfo::exists(c))
    {
      g_GlslangPath = c;
      return c;
    }
  }
  return QString();
}

// —— stage → glslang -S 参数 ——
static const char *StageToArg(GLSLStage s)
{
  switch(s)
  {
    case GLSLStage::Vertex:         return "vert";
    case GLSLStage::Fragment:       return "frag";
    case GLSLStage::Compute:        return "comp";
    case GLSLStage::Geometry:       return "geom";
    case GLSLStage::TessControl:    return "tesc";
    case GLSLStage::TessEvaluation: return "tese";
  }
  return "frag";
}

// —— target env → --target-env 参数 ——
static QString EnvToArg(GLSLTargetEnv env)
{
  switch(env)
  {
    case GLSLTargetEnv::GLES_3_0:    return QStringLiteral("opengl");     // 走 --client opengl
    case GLSLTargetEnv::GLES_3_1:    return QStringLiteral("opengl");
    case GLSLTargetEnv::GLES_3_2:    return QStringLiteral("opengl");
    case GLSLTargetEnv::OpenGL_4_5:  return QStringLiteral("opengl");
    case GLSLTargetEnv::Vulkan_1_0:  return QStringLiteral("vulkan1.0");
    case GLSLTargetEnv::Vulkan_1_1:  return QStringLiteral("vulkan1.1");
    case GLSLTargetEnv::Vulkan_1_2:  return QStringLiteral("vulkan1.2");
    case GLSLTargetEnv::Vulkan_1_3:  return QStringLiteral("vulkan1.3");
  }
  return QStringLiteral("vulkan1.1");
}

static bool IsGLES(GLSLTargetEnv env)
{
  return env == GLSLTargetEnv::GLES_3_0 || env == GLSLTargetEnv::GLES_3_1 ||
         env == GLSLTargetEnv::GLES_3_2;
}

// —— SPIR-V opcode 二进制解析 ——
// SPIR-V 二进制布局：5 word 头部 + 若干指令
// 每条指令 word[0] = (wordCount << 16) | opcode
//
// spirv.hpp11 定义了 spv::Op 枚举，我们不引用它，用一个内嵌小表把常见 op 映射到名字。

struct SpvOpInfo
{
  uint16_t op;
  const char *name;
};

// 覆盖常见与本工具关心的 opcode（未列出的用 "Op<num>" 显示）
static const SpvOpInfo kSpvOps[] = {
  {0,  "OpNop"},           {1, "OpUndef"},          {3, "OpSource"},           {4, "OpSourceExtension"},
  {5,  "OpName"},          {6, "OpMemberName"},     {7, "OpString"},           {8, "OpLine"},
  {10, "OpExtension"},     {11,"OpExtInstImport"},  {12,"OpExtInst"},          {14,"OpMemoryModel"},
  {15, "OpEntryPoint"},    {16,"OpExecutionMode"},  {17,"OpCapability"},
  {19, "OpTypeVoid"},      {20,"OpTypeBool"},       {21,"OpTypeInt"},          {22,"OpTypeFloat"},
  {23, "OpTypeVector"},    {24,"OpTypeMatrix"},     {25,"OpTypeImage"},        {26,"OpTypeSampler"},
  {27, "OpTypeSampledImage"}, {28,"OpTypeArray"},   {29,"OpTypeRuntimeArray"}, {30,"OpTypeStruct"},
  {32, "OpTypePointer"},   {33,"OpTypeFunction"},
  {41, "OpConstantTrue"},  {42,"OpConstantFalse"},  {43,"OpConstant"},         {44,"OpConstantComposite"},
  {46, "OpConstantNull"},
  {48, "OpSpecConstantTrue"}, {49,"OpSpecConstantFalse"}, {50,"OpSpecConstant"},
  {51, "OpSpecConstantComposite"}, {52,"OpSpecConstantOp"},
  {54, "OpFunction"},      {55,"OpFunctionParameter"},   {56,"OpFunctionEnd"}, {57,"OpFunctionCall"},
  {59, "OpVariable"},      {60,"OpImageTexelPointer"},
  {61, "OpLoad"},          {62,"OpStore"},          {63,"OpCopyMemory"},
  {65, "OpAccessChain"},   {66,"OpInBoundsAccessChain"}, {67,"OpPtrAccessChain"},
  {68, "OpArrayLength"},
  {71, "OpDecorate"},      {72,"OpMemberDecorate"},
  {77, "OpVectorExtractDynamic"}, {78,"OpVectorInsertDynamic"}, {79,"OpVectorShuffle"},
  {80, "OpCompositeConstruct"},   {81,"OpCompositeExtract"},    {82,"OpCompositeInsert"},
  {83, "OpCopyObject"},    {84,"OpTranspose"},
  {86, "OpSampledImage"},
  {87, "OpImageSampleImplicitLod"}, {88,"OpImageSampleExplicitLod"},
  {89, "OpImageSampleDrefImplicitLod"}, {90,"OpImageSampleDrefExplicitLod"},
  {91, "OpImageSampleProjImplicitLod"}, {92,"OpImageSampleProjExplicitLod"},
  {93, "OpImageSampleProjDrefImplicitLod"}, {94,"OpImageSampleProjDrefExplicitLod"},
  {95, "OpImageFetch"},    {96,"OpImageGather"},    {97,"OpImageDrefGather"},
  {98, "OpImageRead"},     {99,"OpImageWrite"},     {100,"OpImage"},
  {103,"OpImageQuerySizeLod"}, {104,"OpImageQuerySize"}, {105,"OpImageQueryLod"},
  {106,"OpImageQueryLevels"}, {107,"OpImageQuerySamples"},
  {109,"OpConvertFToU"},   {110,"OpConvertFToS"},   {111,"OpConvertSToF"},     {112,"OpConvertUToF"},
  {113,"OpUConvert"},      {114,"OpSConvert"},      {115,"OpFConvert"},        {116,"OpQuantizeToF16"},
  {124,"OpBitcast"},
  {126,"OpSNegate"},       {127,"OpFNegate"},
  {128,"OpIAdd"},          {129,"OpFAdd"},          {130,"OpISub"},            {131,"OpFSub"},
  {132,"OpIMul"},          {133,"OpFMul"},          {134,"OpUDiv"},            {135,"OpSDiv"},
  {136,"OpFDiv"},          {137,"OpUMod"},          {138,"OpSRem"},            {139,"OpSMod"},
  {140,"OpFRem"},          {141,"OpFMod"},
  {142,"OpVectorTimesScalar"}, {143,"OpMatrixTimesScalar"}, {144,"OpVectorTimesMatrix"},
  {145,"OpMatrixTimesVector"}, {146,"OpMatrixTimesMatrix"}, {147,"OpOuterProduct"}, {148,"OpDot"},
  {149,"OpIAddCarry"},     {150,"OpISubBorrow"},    {151,"OpUMulExtended"},    {152,"OpSMulExtended"},
  {154,"OpAny"},           {155,"OpAll"},           {156,"OpIsNan"},           {157,"OpIsInf"},
  {164,"OpLogicalEqual"},  {165,"OpLogicalNotEqual"}, {166,"OpLogicalOr"},     {167,"OpLogicalAnd"},
  {168,"OpLogicalNot"},    {169,"OpSelect"},
  {170,"OpIEqual"},        {171,"OpINotEqual"},
  {172,"OpUGreaterThan"},  {173,"OpSGreaterThan"},
  {174,"OpUGreaterThanEqual"}, {175,"OpSGreaterThanEqual"},
  {176,"OpULessThan"},     {177,"OpSLessThan"},
  {178,"OpULessThanEqual"}, {179,"OpSLessThanEqual"},
  {180,"OpFOrdEqual"},     {181,"OpFUnordEqual"},
  {182,"OpFOrdNotEqual"},  {183,"OpFUnordNotEqual"},
  {184,"OpFOrdLessThan"},  {185,"OpFUnordLessThan"},
  {186,"OpFOrdGreaterThan"}, {187,"OpFUnordGreaterThan"},
  {188,"OpFOrdLessThanEqual"}, {189,"OpFUnordLessThanEqual"},
  {190,"OpFOrdGreaterThanEqual"}, {191,"OpFUnordGreaterThanEqual"},
  {194,"OpShiftRightLogical"}, {195,"OpShiftRightArithmetic"}, {196,"OpShiftLeftLogical"},
  {197,"OpBitwiseOr"},     {198,"OpBitwiseXor"},    {199,"OpBitwiseAnd"},      {200,"OpNot"},
  {201,"OpBitFieldInsert"}, {202,"OpBitFieldSExtract"}, {203,"OpBitFieldUExtract"},
  {204,"OpBitReverse"},    {205,"OpBitCount"},
  {207,"OpDPdx"},          {208,"OpDPdy"},          {209,"OpFwidth"},
  {210,"OpDPdxFine"},      {211,"OpDPdyFine"},      {212,"OpFwidthFine"},
  {213,"OpDPdxCoarse"},    {214,"OpDPdyCoarse"},    {215,"OpFwidthCoarse"},
  {218,"OpEmitVertex"},    {219,"OpEndPrimitive"},
  {224,"OpControlBarrier"}, {225,"OpMemoryBarrier"},
  {227,"OpAtomicLoad"},    {228,"OpAtomicStore"},   {229,"OpAtomicExchange"},  {230,"OpAtomicCompareExchange"},
  {232,"OpAtomicIIncrement"}, {233,"OpAtomicIDecrement"}, {234,"OpAtomicIAdd"}, {235,"OpAtomicISub"},
  {236,"OpAtomicSMin"},    {237,"OpAtomicUMin"},    {238,"OpAtomicSMax"},      {239,"OpAtomicUMax"},
  {240,"OpAtomicAnd"},     {241,"OpAtomicOr"},      {242,"OpAtomicXor"},
  {245,"OpPhi"},           {246,"OpLoopMerge"},     {247,"OpSelectionMerge"},  {248,"OpLabel"},
  {249,"OpBranch"},        {250,"OpBranchConditional"}, {251,"OpSwitch"},      {252,"OpKill"},
  {253,"OpReturn"},        {254,"OpReturnValue"},   {255,"OpUnreachable"},
  {317,"OpNoLine"},
  {4416,"OpTerminateInvocation"},
};

static QString SpvOpName(uint16_t op)
{
  for(const SpvOpInfo &i : kSpvOps)
    if(i.op == op)
      return QString::fromLatin1(i.name);
  return QStringLiteral("Op") + QString::number(op);
}

// —— GLSL.std.450 常见扩展指令映射 ——
// 参考 GLSL.std.450.h
static QString GlslStd450Name(uint32_t inst)
{
  static const char *names[] = {
    nullptr, "Round","RoundEven","Trunc","FAbs","SAbs","FSign","SSign",
    "Floor","Ceil","Fract","Radians","Degrees","Sin","Cos","Tan",
    "Asin","Acos","Atan","Sinh","Cosh","Tanh","Asinh","Acosh",
    "Atanh","Atan2","Pow","Exp","Log","Exp2","Log2","Sqrt",
    "InverseSqrt","Determinant","MatrixInverse","Modf","ModfStruct",
    "FMin","UMin","SMin","FMax","UMax","SMax","FClamp","UClamp","SClamp",
    "FMix","IMix","Step","SmoothStep","Fma","Frexp","FrexpStruct","Ldexp",
    "PackSnorm4x8","PackUnorm4x8","PackSnorm2x16","PackUnorm2x16","PackHalf2x16",
    "PackDouble2x32","UnpackSnorm2x16","UnpackUnorm2x16","UnpackHalf2x16",
    "UnpackSnorm4x8","UnpackUnorm4x8","UnpackDouble2x32","Length","Distance",
    "Cross","Normalize","FaceForward","Reflect","Refract","FindILsb",
    "FindSMsb","FindUMsb","InterpolateAtCentroid","InterpolateAtSample",
    "InterpolateAtOffset","NMin","NMax","NClamp",
  };
  if(inst < sizeof(names)/sizeof(names[0]) && names[inst])
    return QString::fromLatin1(names[inst]);
  return QStringLiteral("Ext") + QString::number(inst);
}

static OpcodeCategory ClassifySpvOp(const QString &name)
{
  // 声明 / 元数据
  static const QSet<QString> decl = {
    QStringLiteral("OpNop"),          QStringLiteral("OpSource"),
    QStringLiteral("OpSourceExtension"), QStringLiteral("OpName"),
    QStringLiteral("OpMemberName"),   QStringLiteral("OpString"),
    QStringLiteral("OpLine"),         QStringLiteral("OpNoLine"),
    QStringLiteral("OpExtension"),    QStringLiteral("OpExtInstImport"),
    QStringLiteral("OpMemoryModel"),  QStringLiteral("OpEntryPoint"),
    QStringLiteral("OpExecutionMode"), QStringLiteral("OpCapability"),
    QStringLiteral("OpDecorate"),     QStringLiteral("OpMemberDecorate"),
    QStringLiteral("OpFunction"),     QStringLiteral("OpFunctionEnd"),
    QStringLiteral("OpFunctionParameter"),
    QStringLiteral("OpVariable"),     QStringLiteral("OpConstant"),
    QStringLiteral("OpConstantTrue"), QStringLiteral("OpConstantFalse"),
    QStringLiteral("OpConstantComposite"), QStringLiteral("OpConstantNull"),
    QStringLiteral("OpSpecConstant"), QStringLiteral("OpSpecConstantTrue"),
    QStringLiteral("OpSpecConstantFalse"),
    QStringLiteral("OpSpecConstantComposite"),QStringLiteral("OpSpecConstantOp"),
    QStringLiteral("OpUndef"),
  };
  if(decl.contains(name))
    return OpcodeCategory::Decl;
  if(name.startsWith(QStringLiteral("OpType")))
    return OpcodeCategory::Decl;

  if(name.startsWith(QStringLiteral("OpAtomic")))
    return OpcodeCategory::Atomic;
  if(name.startsWith(QStringLiteral("OpDPd")) ||
     name.startsWith(QStringLiteral("OpFwidth")))
    return OpcodeCategory::Derivative;
  if(name.startsWith(QStringLiteral("OpConvert")) ||
     name == QStringLiteral("OpUConvert") ||
     name == QStringLiteral("OpSConvert") ||
     name == QStringLiteral("OpFConvert") ||
     name == QStringLiteral("OpBitcast") ||
     name == QStringLiteral("OpQuantizeToF16"))
    return OpcodeCategory::Convert;

  static const QSet<QString> bitOps = {
    QStringLiteral("OpBitwiseAnd"),   QStringLiteral("OpBitwiseOr"),
    QStringLiteral("OpBitwiseXor"),   QStringLiteral("OpNot"),
    QStringLiteral("OpShiftLeftLogical"),
    QStringLiteral("OpShiftRightLogical"),
    QStringLiteral("OpShiftRightArithmetic"),
    QStringLiteral("OpBitFieldInsert"),QStringLiteral("OpBitFieldSExtract"),
    QStringLiteral("OpBitFieldUExtract"),
    QStringLiteral("OpBitReverse"),   QStringLiteral("OpBitCount"),
  };
  if(bitOps.contains(name))
    return OpcodeCategory::Bitwise;

  static const QSet<QString> ctrl = {
    QStringLiteral("OpLabel"),  QStringLiteral("OpBranch"),
    QStringLiteral("OpBranchConditional"), QStringLiteral("OpSwitch"),
    QStringLiteral("OpLoopMerge"), QStringLiteral("OpSelectionMerge"),
    QStringLiteral("OpReturn"), QStringLiteral("OpReturnValue"),
    QStringLiteral("OpKill"),   QStringLiteral("OpTerminateInvocation"),
    QStringLiteral("OpUnreachable"), QStringLiteral("OpFunctionCall"),
    QStringLiteral("OpPhi"),    QStringLiteral("OpControlBarrier"),
    QStringLiteral("OpMemoryBarrier"),
    QStringLiteral("OpEmitVertex"), QStringLiteral("OpEndPrimitive"),
  };
  if(ctrl.contains(name))
    return OpcodeCategory::Control;

  static const QSet<QString> mem = {
    QStringLiteral("OpLoad"),   QStringLiteral("OpStore"),
    QStringLiteral("OpCopyMemory"),
    QStringLiteral("OpAccessChain"),QStringLiteral("OpInBoundsAccessChain"),
    QStringLiteral("OpPtrAccessChain"),QStringLiteral("OpArrayLength"),
    QStringLiteral("OpImageTexelPointer"),
  };
  if(mem.contains(name))
    return OpcodeCategory::Memory;
  if(name.startsWith(QStringLiteral("OpImage")))
    return OpcodeCategory::Memory;
  if(name == QStringLiteral("OpSampledImage"))
    return OpcodeCategory::Memory;

  if(name.startsWith(QStringLiteral("Ext.")))
    return OpcodeCategory::ExtInst;

  // 剩下的（IAdd / FAdd / FMul / Dot / FOrdEqual / OpSelect / OpAny / OpAll / Composite* / VectorShuffle …）
  return OpcodeCategory::ALU;
}

// 从 SPIR-V 二进制统计所有指令
static void ParseSpirvBinary(const QByteArray &binary, HLSLAnalyzeResult &out)
{
  if(binary.size() < 20)
    return;
  const uint32_t *words = reinterpret_cast<const uint32_t *>(binary.constData());
  size_t wordCount = binary.size() / 4;

  // Magic check
  if(words[0] != 0x07230203)
  {
    out.errors += QObject::tr("\nSPIR-V magic 校验失败。");
    return;
  }

  // 跳过 5 word 头（magic / version / generator / bound / schema）
  size_t i = 5;
  QMap<QString, int> counts;
  uint32_t glslStd450Set = 0;  // OpExtInstImport 的 id，用于识别 OpExtInst 的目标
  while(i < wordCount)
  {
    uint32_t w0 = words[i];
    uint16_t wc = (uint16_t)(w0 >> 16);
    uint16_t op = (uint16_t)(w0 & 0xFFFF);
    if(wc == 0 || i + wc > wordCount)
      break;

    QString name = SpvOpName(op);

    // 记录 GLSL.std.450 的 id
    if(op == 11 /*OpExtInstImport*/ && wc >= 3)
    {
      const char *lit = reinterpret_cast<const char *>(&words[i + 2]);
      // words[i+1] = id, 后续为字符串
      if(strstr(lit, "GLSL.std.450"))
        glslStd450Set = words[i + 1];
    }

    // 展开 OpExtInst → Ext.<Name>
    if(op == 12 /*OpExtInst*/ && wc >= 5)
    {
      uint32_t setId = words[i + 3];
      uint32_t inst  = words[i + 4];
      if(setId == glslStd450Set && glslStd450Set != 0)
        name = QStringLiteral("Ext.") + GlslStd450Name(inst);
    }

    OpcodeCategory cat = ClassifySpvOp(name);
    out.categoryCounts[cat] += 1;
    if(cat != OpcodeCategory::Decl)
      out.totalOpcodes += 1;
    counts[name] += 1;

    i += wc;
  }

  QVector<QPair<QString, int>> pairs;
  pairs.reserve(counts.size());
  for(auto it = counts.constBegin(); it != counts.constEnd(); ++it)
    pairs.push_back(qMakePair(it.key(), it.value()));

  std::sort(pairs.begin(), pairs.end(),
            [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
              if(a.second != b.second)
                return a.second > b.second;
              return a.first < b.first;
            });
  const int TOP_N = 30;
  if(pairs.size() > TOP_N)
    pairs.resize(TOP_N);
  out.topOpcodes = pairs;
}

HLSLAnalyzeResult AnalyzeGLSL(const QString &source, GLSLStage stage, GLSLTargetEnv env,
                              const QString &entry, const QStringList &defines, HLSLOptLevel opt)
{
  HLSLAnalyzeResult result;
  result.lang = ShaderSourceLang::GLSL;

  QString exe = LocateGlslangValidator();
  if(exe.isEmpty())
  {
    result.errors = QObject::tr(
        "未找到 glslangValidator.exe。\n"
        "已搜索以下位置：\n"
        "  - C:/Program Files/RenderDoc/plugins/spirv/\n"
        "  - qrenderdoc 目录下 plugins/spirv/\n"
        "  - %VULKAN_SDK%/Bin/\n"
        "  - PATH\n"
        "请在面板中手动指定路径。");
    return result;
  }

  QTemporaryDir tmp;
  if(!tmp.isValid())
  {
    result.errors = QObject::tr("无法创建临时目录");
    return result;
  }

  // 后缀决定 glslang 自动 stage 推断，但我们显式传 -S 更保险
  QString srcExt;
  switch(stage)
  {
    case GLSLStage::Vertex:   srcExt = QStringLiteral(".vert"); break;
    case GLSLStage::Fragment: srcExt = QStringLiteral(".frag"); break;
    case GLSLStage::Compute:  srcExt = QStringLiteral(".comp"); break;
    case GLSLStage::Geometry: srcExt = QStringLiteral(".geom"); break;
    case GLSLStage::TessControl:    srcExt = QStringLiteral(".tesc"); break;
    case GLSLStage::TessEvaluation: srcExt = QStringLiteral(".tese"); break;
  }
  QString srcPath = tmp.path() + QStringLiteral("/src") + srcExt;
  QString outPath = tmp.path() + QStringLiteral("/out.spv");

  {
    QFile f(srcPath);
    if(!f.open(QIODevice::WriteOnly))
    {
      result.errors = QObject::tr("无法写入临时源文件");
      return result;
    }
    f.write(source.toUtf8());
  }

  QStringList args;
  args << QStringLiteral("-V");                     // 输出 SPIR-V 二进制
  args << QStringLiteral("-S") << QLatin1String(StageToArg(stage));
  args << QStringLiteral("-o") << outPath;

  // client / target-env
  if(IsGLES(env))
  {
    args << QStringLiteral("--client") << QStringLiteral("opengl100");
    args << QStringLiteral("--target-env") << QStringLiteral("opengl");
  }
  else if(env == GLSLTargetEnv::OpenGL_4_5)
  {
    args << QStringLiteral("--client") << QStringLiteral("opengl100");
    args << QStringLiteral("--target-env") << QStringLiteral("opengl");
  }
  else
  {
    args << QStringLiteral("--target-env") << EnvToArg(env);
  }

  // 优化：glslang 自身只有 -Os（size 优化）与默认。 O0 用 -Od
  switch(opt)
  {
    case HLSLOptLevel::O0: args << QStringLiteral("-Od"); break;
    case HLSLOptLevel::O1: /* 默认 */ break;
    case HLSLOptLevel::O2: args << QStringLiteral("-Os"); break;
    case HLSLOptLevel::O3: args << QStringLiteral("-Os"); break;
  }

  // entry point（HLSL 才有意义；GLSL 默认 main）
  if(!entry.isEmpty() && entry != QStringLiteral("main"))
    args << QStringLiteral("--source-entrypoint") << entry;

  // defines
  for(const QString &d : defines)
  {
    QString kv = d.trimmed();
    if(kv.isEmpty())
      continue;
    args << QStringLiteral("-D") + kv;
  }

  args << srcPath;

  QProcess proc;
  proc.start(exe, args);
  if(!proc.waitForStarted(5000))
  {
    result.errors = QObject::tr("无法启动 glslangValidator");
    return result;
  }
  if(!proc.waitForFinished(30000))
  {
    proc.kill();
    result.errors = QObject::tr("glslangValidator 超时");
    return result;
  }

  QString stdOut = QString::fromLocal8Bit(proc.readAllStandardOutput());
  QString stdErr = QString::fromLocal8Bit(proc.readAllStandardError());
  QString combined = stdOut + stdErr;

  if(proc.exitCode() != 0 || !QFileInfo::exists(outPath))
  {
    result.errors = QObject::tr("glslangValidator 编译失败：\n") + combined;
    return result;
  }

  // 读取 SPIR-V 二进制
  QByteArray spv;
  {
    QFile f(outPath);
    if(!f.open(QIODevice::ReadOnly))
    {
      result.errors = QObject::tr("无法读取生成的 SPIR-V");
      return result;
    }
    spv = f.readAll();
  }

  // 统计
  ParseSpirvBinary(spv, result);

  // 生成可读反汇编：优先调 spirv-dis.exe（与 glslangValidator 常同目录）
  QString spirvDis =
      QFileInfo(exe).absolutePath() + QStringLiteral("/spirv-dis.exe");
  if(QFileInfo::exists(spirvDis))
  {
    QProcess dis;
    dis.start(spirvDis, QStringList() << outPath);
    if(dis.waitForFinished(15000))
      result.disassembly = QString::fromLocal8Bit(dis.readAllStandardOutput());
  }

  // 兜底：给出精简的 opcode 列表
  if(result.disassembly.isEmpty())
  {
    QStringList lines;
    lines << QStringLiteral("; SPIR-V 简易反汇编（未找到 spirv-dis.exe，仅列 opcode 与出现顺序）");
    lines << QStringLiteral("; 若需完整反汇编，请把 spirv-dis.exe 放到 glslangValidator.exe 同目录");
    const uint32_t *words = reinterpret_cast<const uint32_t *>(spv.constData());
    size_t wc = spv.size() / 4;
    size_t i = 5;
    int idx = 0;
    while(i < wc && idx < 5000)
    {
      uint32_t w0 = words[i];
      uint16_t sz = (uint16_t)(w0 >> 16);
      uint16_t op = (uint16_t)(w0 & 0xFFFF);
      if(sz == 0 || i + sz > wc)
        break;
      lines << QStringLiteral("%1: %2  (words=%3)")
                   .arg(idx, 5).arg(SpvOpName(op)).arg(sz);
      i += sz;
      idx++;
    }
    result.disassembly = lines.join(QLatin1Char('\n'));
  }

  if(!combined.trimmed().isEmpty())
    result.errors = combined;

  result.compileOk = true;
  result.instructionSlots = result.totalOpcodes;   // SPIR-V 没有 slot 概念，用 opcode 数
  return result;
}

