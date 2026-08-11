/******************************************************************************
 * HLSL 分析器 UI 面板 实现
 ******************************************************************************/

#include "HLSLAnalyzerPanel.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QTabWidget>
#include <QFont>
#include <QApplication>

HLSLAnalyzerPanel::HLSLAnalyzerPanel(QWidget *parent) : QFrame(parent)
{
  setWindowTitle(tr("DXBC 指令统计"));
  buildUI();
}

HLSLAnalyzerPanel::~HLSLAnalyzerPanel() = default;

void HLSLAnalyzerPanel::buildUI()
{
  QVBoxLayout *root = new QVBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  root->setSpacing(4);

  // ---- 顶部：编译选项 ----
  QGroupBox *optBox = new QGroupBox(tr("编译选项"), this);
  QGridLayout *g = new QGridLayout(optBox);
  g->setContentsMargins(6, 6, 6, 6);
  g->setSpacing(4);

  g->addWidget(new QLabel(tr("目标 Profile:")), 0, 0);
  m_ProfileCombo = new QComboBox(this);
  m_ProfileCombo->addItems({
      QStringLiteral("ps_5_0"), QStringLiteral("vs_5_0"), QStringLiteral("cs_5_0"),
      QStringLiteral("gs_5_0"), QStringLiteral("hs_5_0"), QStringLiteral("ds_5_0"),
      QStringLiteral("ps_5_1"), QStringLiteral("vs_5_1"), QStringLiteral("cs_5_1"),
      QStringLiteral("ps_4_0"), QStringLiteral("vs_4_0"), QStringLiteral("cs_4_0"),
  });
  g->addWidget(m_ProfileCombo, 0, 1);

  g->addWidget(new QLabel(tr("入口函数:")), 0, 2);
  m_EntryEdit = new QLineEdit(QStringLiteral("main"), this);
  m_EntryEdit->setMinimumWidth(120);
  g->addWidget(m_EntryEdit, 0, 3);

  g->addWidget(new QLabel(tr("优化级别:")), 0, 4);
  m_OptCombo = new QComboBox(this);
  m_OptCombo->addItem(tr("/O3 最大优化"), (int)HLSLOptLevel::O3);
  m_OptCombo->addItem(tr("/O2"),          (int)HLSLOptLevel::O2);
  m_OptCombo->addItem(tr("/O1"),          (int)HLSLOptLevel::O1);
  m_OptCombo->addItem(tr("/Od 禁用优化"), (int)HLSLOptLevel::O0);
  g->addWidget(m_OptCombo, 0, 5);

  m_AnalyzeBtn = new QPushButton(tr("分析当前源码"), this);
  m_AnalyzeBtn->setDefault(true);
  g->addWidget(m_AnalyzeBtn, 0, 6);

  g->addWidget(new QLabel(tr("宏定义 Defines（每行一个，KEY 或 KEY=VAL）:")), 1, 0, 1, 7);
  m_DefinesEdit = new QPlainTextEdit(this);
  m_DefinesEdit->setMaximumHeight(80);
  QFont mono = QFont(QStringLiteral("Consolas"));
  mono.setStyleHint(QFont::Monospace);
  m_DefinesEdit->setFont(mono);
  g->addWidget(m_DefinesEdit, 2, 0, 1, 7);

  g->setColumnStretch(3, 1);
  root->addWidget(optBox);

  // ---- 中部：状态 + 统计 ----
  m_StatusLabel = new QLabel(tr("尚未分析。点击「分析当前源码」以编译并统计。"), this);
  m_StatusLabel->setWordWrap(true);
  root->addWidget(m_StatusLabel);

  // 统计 + 反汇编 双 tab
  QTabWidget *tabs = new QTabWidget(this);

  m_StatsText = new QTextEdit(this);
  m_StatsText->setReadOnly(true);
  m_StatsText->setFont(mono);
  m_StatsText->setLineWrapMode(QTextEdit::NoWrap);
  tabs->addTab(m_StatsText, tr("统计信息"));

  m_DisasmText = new QPlainTextEdit(this);
  m_DisasmText->setReadOnly(true);
  m_DisasmText->setFont(mono);
  m_DisasmText->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_DisasmText->setPlaceholderText(tr("反汇编将在编译成功后显示于此。"));
  tabs->addTab(m_DisasmText, tr("反汇编 (DXBC)"));

  root->addWidget(tabs, 1);

  connect(m_AnalyzeBtn, &QPushButton::clicked, this, &HLSLAnalyzerPanel::onAnalyzeClicked);
}

void HLSLAnalyzerPanel::SetDefaults(const QString &entry, const QString &profile)
{
  if(!entry.isEmpty())
    m_EntryEdit->setText(entry);
  if(!profile.isEmpty())
  {
    int idx = m_ProfileCombo->findText(profile);
    if(idx >= 0)
      m_ProfileCombo->setCurrentIndex(idx);
    else
    {
      m_ProfileCombo->insertItem(0, profile);
      m_ProfileCombo->setCurrentIndex(0);
    }
  }
}

void HLSLAnalyzerPanel::onAnalyzeClicked()
{
  if(!m_SourceProvider)
  {
    m_StatusLabel->setText(tr("错误：未绑定源码提供器。"));
    return;
  }

  QString source = m_SourceProvider();
  if(source.isEmpty())
  {
    m_StatusLabel->setText(tr("错误：源码为空。"));
    return;
  }

  QStringList defines;
  for(const QString &line : m_DefinesEdit->toPlainText().split(QLatin1Char('\n')))
  {
    QString t = line.trimmed();
    if(!t.isEmpty())
      defines.push_back(t);
  }

  HLSLOptLevel opt = (HLSLOptLevel)m_OptCombo->currentData().toInt();

  m_StatusLabel->setText(tr("编译中，请稍候..."));
  QApplication::processEvents();

  HLSLAnalyzeResult r =
      AnalyzeHLSL(source, m_ProfileCombo->currentText(), m_EntryEdit->text(), defines, opt);

  showResult(r);
}

void HLSLAnalyzerPanel::showResult(const HLSLAnalyzeResult &r)
{
  if(!r.compileOk)
  {
    m_StatusLabel->setText(tr("编译失败。"));
    m_StatsText->setPlainText(r.errors);
    m_DisasmText->setPlainText(QString());
    return;
  }

  m_DisasmText->setPlainText(r.disassembly);

  m_StatusLabel->setText(
      tr("编译成功。Instruction Slots = %1 ; 可执行 opcode 数 = %2")
          .arg(r.instructionSlots)
          .arg(r.totalOpcodes));

  QString out;
  out += tr("========== 核心指标 ==========\n");
  out += tr("Instruction Slots（指令槽数，fxc 官方报告）: %1\n").arg(r.instructionSlots);
  out += tr("  -> D3D 虚拟 ISA 的指令槽数，评估 shader 复杂度的主要参考。\n");
  out += tr("  -> 部分复合指令占多槽（如 sincos=2），纹理采样只算 1 槽但实际很贵。\n\n");

  out += tr("Counted Opcodes（本工具计数的可执行指令数）: %1\n").arg(r.totalOpcodes);
  out += tr("  -> 逐行统计的 opcode 数量，已排除声明与注释，供分类分析。\n");
  out += tr("  -> 与官方槽数通常略有差异，属正常现象。\n\n");

  out += tr("========== 按类别统计 ==========\n");
  static const OpcodeCategory order[] = {
      OpcodeCategory::ALU,        OpcodeCategory::Memory,   OpcodeCategory::Control,
      OpcodeCategory::Atomic,     OpcodeCategory::Derivative, OpcodeCategory::Convert,
      OpcodeCategory::Bitwise,    OpcodeCategory::Other,    OpcodeCategory::Decl,
  };
  for(OpcodeCategory c : order)
  {
    int v = r.categoryCounts.value(c, 0);
    if(c == OpcodeCategory::Decl)
    {
      out += QStringLiteral("  [%1]         %2\n")
                 .arg(v, 4)
                 .arg(CategoryLabel(c));
    }
    else
    {
      double pct = (r.totalOpcodes > 0) ? (v * 100.0 / r.totalOpcodes) : 0.0;
      out += QStringLiteral("  [%1] (%2%)  %3\n")
                 .arg(v, 4)
                 .arg(pct, 5, 'f', 1)
                 .arg(CategoryLabel(c));
    }
  }
  out += QLatin1Char('\n');

  out += tr("========== Opcode 使用频次 Top 30 ==========\n");
  out += QStringLiteral("  %1 %2  %3\n")
             .arg(tr("指令"), -24)
             .arg(tr("次数"), 6)
             .arg(tr("说明"));
  out += QStringLiteral("  %1 %2  %3\n")
             .arg(QString(24, QLatin1Char('-')))
             .arg(QString(6,  QLatin1Char('-')))
             .arg(QString(50, QLatin1Char('-')));
  for(const QPair<QString, int> &p : r.topOpcodes)
  {
    out += QStringLiteral("  %1 %2  %3\n")
               .arg(p.first, -24)
               .arg(p.second, 6)
               .arg(LookupOpcodeHelp(p.first));
  }

  if(!r.errors.isEmpty())
  {
    out += tr("\n========== 编译器警告 ==========\n");
    out += r.errors;
  }

  m_StatsText->setPlainText(out);
}
