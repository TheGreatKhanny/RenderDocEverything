/******************************************************************************
 * HLSL 分析器 UI 面板 —— 集成到 ShaderViewer 的 dock 面板
 ******************************************************************************/

#pragma once

#include <QFrame>
#include "HLSLAnalyzer.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTextEdit;
class QPushButton;
class QLabel;
class ScintillaEdit;

class HLSLAnalyzerPanel : public QFrame
{
  Q_OBJECT
public:
  explicit HLSLAnalyzerPanel(QWidget *parent = nullptr);
  ~HLSLAnalyzerPanel();

  // 由 ShaderViewer 调用，注入源码获取回调（点击"分析"时调用）
  typedef std::function<QString()> SourceProvider;
  void SetSourceProvider(SourceProvider fn) { m_SourceProvider = fn; }

  // 由 ShaderViewer 调用，注入入口点/target profile 默认值
  void SetDefaults(const QString &entry, const QString &profile);

  // 由 ShaderViewer 调用：告诉面板当前源码是 HLSL 还是 GLSL
  void SetSourceLanguage(ShaderSourceLang lang);
  // 由 ShaderViewer 调用：告诉面板 shader stage（用于 GLSL 分支）
  void SetStage(GLSLStage stage);

private slots:
  void onAnalyzeClicked();
  void onLocateGlslangClicked();

private:
  void buildUI();
  void showResult(const HLSLAnalyzeResult &r);
  void updateVisibilityForLang();

  QComboBox     *m_LangCombo      = nullptr;     // HLSL / GLSL
  QComboBox     *m_ProfileCombo   = nullptr;     // HLSL profile
  QComboBox     *m_GlslStageCombo = nullptr;     // GLSL stage
  QComboBox     *m_GlslEnvCombo   = nullptr;     // GLSL target env
  QLineEdit     *m_EntryEdit      = nullptr;
  QComboBox     *m_OptCombo       = nullptr;
  QPlainTextEdit*m_DefinesEdit    = nullptr;
  QPushButton   *m_AnalyzeBtn     = nullptr;
  QPushButton   *m_LocateGlslangBtn = nullptr;
  QLabel        *m_StatusLabel    = nullptr;
  QTextEdit     *m_StatsText      = nullptr;
  QPlainTextEdit*m_DisasmText     = nullptr;

  ShaderSourceLang m_Lang = ShaderSourceLang::HLSL;
  GLSLStage m_GlslStage = GLSLStage::Fragment;

  SourceProvider m_SourceProvider;
};
