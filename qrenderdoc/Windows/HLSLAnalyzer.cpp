/******************************************************************************
 * HLSL DXBC 指令统计分析器 — 核心实现
 ******************************************************************************/

#include "HLSLAnalyzer.h"

#include <QRegularExpression>
#include <QLibrary>
#include <QFileInfo>

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

  return m;
}

QString LookupOpcodeHelp(const QString &opcode)
{
  static const QMap<QString, QString> table = BuildOpcodeHelp();

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

  return QObject::tr("未收录指令，请查阅 DXBC 文档");
}
