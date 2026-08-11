"""
HLSL -> Optimized DXBC Analyzer
Single-file tkinter tool. Auto-locates fxc.exe, compiles HLSL with /O3,
shows disassembly and instruction statistics.

Usage: python hlsl_analyzer.py
"""

import os
import re
import sys
import glob
import tempfile
import subprocess
import threading
from collections import Counter
from pathlib import Path

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext


# ---------- fxc discovery ----------

def find_fxc():
    """Search common Windows SDK locations for fxc.exe (newest first)."""
    candidates = []
    roots = [
        r"C:\Program Files (x86)\Windows Kits\10\bin",
        r"C:\Program Files\Windows Kits\10\bin",
        r"C:\Program Files (x86)\Windows Kits\8.1\bin",
    ]
    for root in roots:
        if not os.path.isdir(root):
            continue
        # bin\10.0.xxxxx.0\x64\fxc.exe
        for p in glob.glob(os.path.join(root, "*", "x64", "fxc.exe")):
            candidates.append(p)
        # bin\x64\fxc.exe (older layout)
        p = os.path.join(root, "x64", "fxc.exe")
        if os.path.isfile(p):
            candidates.append(p)

    # PATH fallback
    for p in os.environ.get("PATH", "").split(os.pathsep):
        cand = os.path.join(p, "fxc.exe")
        if os.path.isfile(cand):
            candidates.append(cand)

    if not candidates:
        return None
    # newest SDK first (lexicographic on path works for 10.0.xxxxx.0)
    candidates.sort(reverse=True)
    return candidates[0]


# ---------- DXBC assembly analysis ----------

# Instruction categories (DXBC / SM5 opcodes, lowercase)
ALU_OPS = {
    "mov", "movc", "mad", "mul", "add", "div", "sub",
    "dp2", "dp3", "dp4", "dp2add",
    "min", "max", "rsq", "rcp", "sqrt", "exp", "log", "log2", "exp2",
    "sin", "cos", "sincos", "tan",
    "frc", "round_ne", "round_ni", "round_pi", "round_z",
    "and", "or", "xor", "not", "ishl", "ishr", "ushr",
    "iadd", "imul", "imad", "imin", "imax", "idiv",
    "eq", "ne", "lt", "ge", "ieq", "ine", "ilt", "ige", "ult", "uge",
    "ftoi", "ftou", "itof", "utof",
    "abs", "neg", "saturate", "f16tof32", "f32tof16",
    "deriv_rtx", "deriv_rty", "deriv_rtx_coarse", "deriv_rty_coarse",
    "deriv_rtx_fine", "deriv_rty_fine",
}

MEM_OPS_PREFIX = ("sample", "ld", "gather", "store", "bufinfo", "resinfo", "lod")
CTRL_OPS = {
    "if", "else", "endif", "loop", "endloop", "break", "breakc",
    "continue", "continuec", "switch", "case", "default", "endswitch",
    "ret", "retc", "discard", "call", "callc", "label",
}
DECL_PREFIX = ("dcl_", "//")

OPCODE_RE = re.compile(r"^\s*([a-z][a-z0-9_]*)", re.IGNORECASE)
SLOTS_RE = re.compile(r"Approximately\s+(\d+)\s+instruction slots used", re.IGNORECASE)
TEX_RE = re.compile(r"//\s*Texture\s+(\S+)", re.IGNORECASE)


def classify(op):
    op = op.lower()
    if op in CTRL_OPS:
        return "control"
    if any(op.startswith(pfx) for pfx in MEM_OPS_PREFIX):
        return "memory"
    if op in ALU_OPS:
        return "alu"
    if any(op.startswith(pfx) for pfx in DECL_PREFIX):
        return "decl"
    return "other"


def analyze_asm(asm_text):
    """Return dict with slot count, category counts, opcode histogram."""
    slots = None
    m = SLOTS_RE.search(asm_text)
    if m:
        slots = int(m.group(1))

    cats = Counter()
    ops = Counter()
    in_code = False
    for line in asm_text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        # Skip pure comments and declarations from the counted body,
        # but still classify them for the histogram.
        m = OPCODE_RE.match(stripped)
        if not m:
            continue
        op = m.group(1).lower()
        # Comment lines start with //, OPCODE_RE will match "//" oddly-guard:
        if stripped.startswith("//"):
            continue
        if op.startswith("dcl_") or op in ("ps_5_0", "vs_5_0", "cs_5_0",
                                          "hs_5_0", "ds_5_0", "gs_5_0"):
            cats["decl"] += 1
            continue
        cats[classify(op)] += 1
        ops[op] += 1

    return {
        "slots": slots,
        "categories": cats,
        "opcodes": ops,
    }


# ---------- fxc invocation ----------

def run_fxc(fxc_path, hlsl_path, profile, entry, opt_flag, defines, includes,
            extra_args):
    """Compile HLSL, return (returncode, asm_text, stderr_text)."""
    with tempfile.TemporaryDirectory() as td:
        asm_out = os.path.join(td, "out.asm")
        obj_out = os.path.join(td, "out.dxbc")
        args = [fxc_path,
                "/nologo",
                f"/T", profile,
                f"/E", entry,
                opt_flag,
                "/Fc", asm_out,
                "/Fo", obj_out]
        for d in defines:
            d = d.strip()
            if d:
                args += ["/D", d]
        for inc in includes:
            inc = inc.strip()
            if inc:
                args += ["/I", inc]
        if extra_args.strip():
            args += extra_args.strip().split()
        args.append(hlsl_path)

        try:
            proc = subprocess.run(args, capture_output=True, text=True,
                                  timeout=60)
        except subprocess.TimeoutExpired:
            return -1, "", "fxc timeout (>60s)"

        asm_text = ""
        if os.path.isfile(asm_out):
            with open(asm_out, "r", encoding="utf-8", errors="replace") as f:
                asm_text = f.read()

        stderr = proc.stdout + "\n" + proc.stderr
        return proc.returncode, asm_text, stderr


# ---------- UI ----------

DEFAULT_HLSL = """// 示例像素着色器，替换为你自己的代码。
Texture2D    tex0    : register(t0);
SamplerState samp0   : register(s0);

cbuffer CB : register(b0) {
    float4 gTint;
    float  gEnableExtra;
};

struct PSIn {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target {
    float4 c = tex0.Sample(samp0, i.uv) * gTint;
    if (gEnableExtra > 0.5) {
        c.rgb = pow(saturate(c.rgb), 2.2);
    }
    return c;
}
"""

PROFILES = [
    "ps_5_0", "vs_5_0", "cs_5_0", "gs_5_0", "hs_5_0", "ds_5_0",
    "ps_5_1", "vs_5_1", "cs_5_1",
    "ps_4_0", "vs_4_0", "cs_4_0",
]

OPT_LEVELS = [
    ("/O3 (max)", "/O3"),
    ("/O2",       "/O2"),
    ("/O1",       "/O1"),
    ("/O0 (none)","/O0"),
    ("/Od (disable opt)", "/Od"),
]


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("HLSL -> 优化后 DXBC 分析器")
        self.geometry("1400x900")

        self.fxc_path = find_fxc()

        self._build_ui()
        self._set_status()

    # ---- layout ----
    def _build_ui(self):
        # Top toolbar
        top = ttk.Frame(self)
        top.pack(side=tk.TOP, fill=tk.X, padx=6, pady=4)

        ttk.Button(top, text="打开 HLSL...", command=self.open_file).pack(side=tk.LEFT)
        ttk.Button(top, text="编译 (F5)", command=self.compile_async).pack(side=tk.LEFT, padx=4)
        ttk.Button(top, text="定位 fxc...", command=self.locate_fxc).pack(side=tk.LEFT)

        ttk.Label(top, text="  Profile 目标:").pack(side=tk.LEFT)
        self.profile_var = tk.StringVar(value="ps_5_0")
        ttk.Combobox(top, values=PROFILES, textvariable=self.profile_var,
                     width=10, state="readonly").pack(side=tk.LEFT)

        ttk.Label(top, text="  入口函数:").pack(side=tk.LEFT)
        self.entry_var = tk.StringVar(value="main")
        ttk.Entry(top, textvariable=self.entry_var, width=12).pack(side=tk.LEFT)

        ttk.Label(top, text="  优化级别:").pack(side=tk.LEFT)
        self.opt_var = tk.StringVar(value="/O3")
        ttk.Combobox(top, values=[o[0] for o in OPT_LEVELS],
                     textvariable=self.opt_display_var if False else tk.StringVar(),
                     width=18, state="readonly")  # placeholder
        # Use a proper mapping:
        self.opt_display_var = tk.StringVar(value=OPT_LEVELS[0][0])
        opt_cb = ttk.Combobox(top, values=[o[0] for o in OPT_LEVELS],
                              textvariable=self.opt_display_var,
                              width=18, state="readonly")
        opt_cb.pack(side=tk.LEFT)

        # Main split: left editor, right options+stats+asm
        main = ttk.Panedwindow(self, orient=tk.HORIZONTAL)
        main.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)

        # Left: HLSL editor
        left = ttk.Frame(main)
        ttk.Label(left, text="HLSL 源码").pack(anchor=tk.W)
        self.src_text = scrolledtext.ScrolledText(left, wrap=tk.NONE,
                                                  font=("Consolas", 10),
                                                  undo=True)
        self.src_text.pack(fill=tk.BOTH, expand=True)
        self.src_text.insert("1.0", DEFAULT_HLSL)
        main.add(left, weight=3)

        # Right: vertical split
        right = ttk.Panedwindow(main, orient=tk.VERTICAL)
        main.add(right, weight=4)

        # Options panel
        opts = ttk.LabelFrame(right, text="编译选项")
        row = ttk.Frame(opts); row.pack(fill=tk.X, padx=4, pady=2)
        ttk.Label(row, text="宏定义 Defines（每行一个，格式 KEY 或 KEY=VAL）:").pack(anchor=tk.W)
        self.defines_text = scrolledtext.ScrolledText(opts, height=4,
                                                     font=("Consolas", 9))
        self.defines_text.pack(fill=tk.X, padx=4, pady=2)

        row = ttk.Frame(opts); row.pack(fill=tk.X, padx=4, pady=2)
        ttk.Label(row, text="Include 头文件搜索目录（每行一个）:").pack(anchor=tk.W)
        self.includes_text = scrolledtext.ScrolledText(opts, height=3,
                                                      font=("Consolas", 9))
        self.includes_text.pack(fill=tk.X, padx=4, pady=2)

        row = ttk.Frame(opts); row.pack(fill=tk.X, padx=4, pady=2)
        ttk.Label(row, text="额外 fxc 参数:").pack(side=tk.LEFT)
        self.extra_var = tk.StringVar(value="")
        ttk.Entry(row, textvariable=self.extra_var).pack(side=tk.LEFT,
                                                        fill=tk.X, expand=True, padx=4)
        right.add(opts, weight=0)

        # Stats panel
        stats_frame = ttk.LabelFrame(right, text="统计信息")
        self.stats_text = scrolledtext.ScrolledText(stats_frame, height=10,
                                                    font=("Consolas", 10),
                                                    state=tk.DISABLED)
        self.stats_text.pack(fill=tk.BOTH, expand=True)
        right.add(stats_frame, weight=1)

        # Assembly panel
        asm_frame = ttk.LabelFrame(right, text="反汇编 (DXBC)")
        self.asm_text = scrolledtext.ScrolledText(asm_frame, wrap=tk.NONE,
                                                  font=("Consolas", 10),
                                                  state=tk.DISABLED)
        self.asm_text.pack(fill=tk.BOTH, expand=True)
        right.add(asm_frame, weight=3)

        # Bottom log
        log_frame = ttk.LabelFrame(self, text="输出日志")
        self.log_text = scrolledtext.ScrolledText(log_frame, height=6,
                                                  font=("Consolas", 9),
                                                  state=tk.DISABLED)
        self.log_text.pack(fill=tk.BOTH, expand=True)
        log_frame.pack(side=tk.BOTTOM, fill=tk.X, padx=6, pady=4)

        # Status bar
        self.status_var = tk.StringVar()
        ttk.Label(self, textvariable=self.status_var, anchor=tk.W,
                  relief=tk.SUNKEN).pack(side=tk.BOTTOM, fill=tk.X)

        # Keybinds
        self.bind("<F5>", lambda e: self.compile_async())
        self.bind("<Control-o>", lambda e: self.open_file())
        self.bind("<Control-s>", lambda e: self.save_file())

    def _set_status(self):
        if self.fxc_path:
            self.status_var.set(f"fxc 路径: {self.fxc_path}")
        else:
            self.status_var.set("未找到 fxc.exe。请点击「定位 fxc...」手动选择。")

    # ---- file ops ----
    def open_file(self):
        p = filedialog.askopenfilename(
            filetypes=[("HLSL", "*.hlsl *.fx *.fxh *.hlsli"), ("所有文件", "*.*")])
        if not p:
            return
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()
        self.src_text.delete("1.0", tk.END)
        self.src_text.insert("1.0", data)
        self.current_path = p
        self.title(f"HLSL 分析器 - {p}")

    def save_file(self):
        p = getattr(self, "current_path", None)
        if not p:
            p = filedialog.asksaveasfilename(defaultextension=".hlsl",
                                             filetypes=[("HLSL", "*.hlsl")])
            if not p:
                return
            self.current_path = p
        with open(p, "w", encoding="utf-8") as f:
            f.write(self.src_text.get("1.0", tk.END))
        self.log(f"已保存: {p}\n")

    def locate_fxc(self):
        p = filedialog.askopenfilename(
            title="定位 fxc.exe",
            filetypes=[("fxc.exe", "fxc.exe"), ("所有文件", "*.*")])
        if p:
            self.fxc_path = p
            self._set_status()

    # ---- compile ----
    def compile_async(self):
        if not self.fxc_path:
            messagebox.showerror("缺少 fxc",
                                 "未找到 fxc.exe。请安装 Windows 10 SDK，"
                                 "或使用「定位 fxc...」手动指定路径。")
            return
        threading.Thread(target=self._compile_worker, daemon=True).start()

    def _get_opt_flag(self):
        disp = self.opt_display_var.get()
        for label, flag in OPT_LEVELS:
            if label == disp:
                return flag
        return "/O3"

    def _compile_worker(self):
        src = self.src_text.get("1.0", tk.END)
        # write source to a temp .hlsl so #include works from includes dirs
        with tempfile.NamedTemporaryFile(mode="w", suffix=".hlsl",
                                         delete=False, encoding="utf-8") as f:
            f.write(src)
            src_path = f.name

        defines = [l for l in self.defines_text.get("1.0", tk.END).splitlines()
                   if l.strip()]
        includes = [l for l in self.includes_text.get("1.0", tk.END).splitlines()
                    if l.strip()]
        try:
            rc, asm, err = run_fxc(self.fxc_path, src_path,
                                   self.profile_var.get(),
                                   self.entry_var.get(),
                                   self._get_opt_flag(),
                                   defines, includes,
                                   self.extra_var.get())
        finally:
            try: os.unlink(src_path)
            except OSError: pass

        self.after(0, self._show_result, rc, asm, err)

    def _show_result(self, rc, asm, err):
        self._set_ro(self.asm_text, asm or "")
        self.log(err.strip() + "\n" if err else "")
        if rc != 0:
            self._set_ro(self.stats_text, f"编译失败（退出码 {rc}）。\n"
                                          "详见「输出日志」面板中的错误信息。")
            return
        stats = analyze_asm(asm)
        self._set_ro(self.stats_text, self._format_stats(stats))

    def _format_stats(self, s):
        # 中文分类名 + 专业术语解释
        CAT_LABELS = {
            "alu":     "ALU（算术逻辑指令：加减乘除、点积、rsq/exp/log 等数学运算，着色器主体计算）",
            "memory":  "Memory（内存访问：纹理采样 sample、缓冲区读写 ld/store，通常延迟最高）",
            "control": "Control（控制流：if/else/loop/ret/discard，可能引起波前发散影响性能）",
            "other":   "Other（未分类：不常见或自定义指令）",
            "decl":    "Decl（声明：dcl_* 输入输出/资源声明，不计入运行时开销）",
        }
        # DXBC SM5 opcode 中文解释表（尽量覆盖全部常见指令）
        # 说明：许多指令带 _sat 后缀（结果饱和到 [0,1]），代码里会自动去掉后缀再查表。
        OP_HELP = {
            # ---- 浮点算术 ----
            "mov":    "寄存器搬运",
            "movc":   "条件搬运（三元选择 c?a:b）",
            "mad":    "乘加融合 a*b+c",
            "mul":    "浮点乘法",
            "add":    "浮点加法",
            "sub":    "浮点减法",
            "div":    "浮点除法",
            "min":    "取较小值",
            "max":    "取较大值",
            "frc":    "取小数部分 x - floor(x)",
            "round_ne":"就近取整（Round to Nearest Even）",
            "round_ni":"向下取整（floor）",
            "round_pi":"向上取整（ceil）",
            "round_z": "向零取整（trunc）",
            # ---- 点积与向量 ----
            "dp2":    "二维点积",
            "dp3":    "三维点积",
            "dp4":    "四维点积",
            "dp2add": "二维点积再加",
            # ---- 数学函数 ----
            "rsq":    "平方根倒数 1/sqrt(x)",
            "rcp":    "倒数 1/x",
            "sqrt":   "平方根",
            "exp":    "指数 2^x",
            "log":    "对数 log2(x)",
            "sincos": "正余弦（占 2 slot）",
            "sin":    "正弦",
            "cos":    "余弦",
            "tan":    "正切",
            # ---- 类型转换 ----
            "ftoi":   "float → int（截断）",
            "ftou":   "float → uint（截断）",
            "itof":   "int → float",
            "utof":   "uint → float",
            "f16tof32":"half → float",
            "f32tof16":"float → half",
            # ---- 整数算术 ----
            "iadd":   "整数加法",
            "isub":   "整数减法",
            "imul":   "整数乘法（32×32→64，取高低）",
            "imad":   "整数乘加",
            "imin":   "有符号整数取小",
            "imax":   "有符号整数取大",
            "idiv":   "有符号整数除法（同时得商与余数）",
            "udiv":   "无符号整数除法",
            "umin":   "无符号整数取小",
            "umax":   "无符号整数取大",
            "umad":   "无符号整数乘加",
            "umul":   "无符号整数乘法",
            "ineg":   "整数取负",
            # ---- 位运算 ----
            "and":    "按位与",
            "or":     "按位或",
            "xor":    "按位异或",
            "not":    "按位取反",
            "ishl":   "算术左移",
            "ishr":   "算术右移（有符号）",
            "ushr":   "逻辑右移（无符号）",
            "bfi":    "位域插入",
            "bfrev":  "位反转",
            "ubfe":   "无符号位域提取",
            "ibfe":   "有符号位域提取",
            "countbits":"位计数（popcount）",
            "firstbit_hi":  "最高位为 1 的位置",
            "firstbit_lo":  "最低位为 1 的位置",
            "firstbit_shi": "最高有效符号位位置",
            # ---- 比较 ----
            "eq":     "浮点等于比较",
            "ne":     "浮点不等比较",
            "lt":     "浮点小于比较",
            "ge":     "浮点大于等于比较",
            "ieq":    "整数等于比较",
            "ine":    "整数不等比较",
            "ilt":    "有符号整数小于比较",
            "ige":    "有符号整数大于等于比较",
            "ult":    "无符号整数小于比较",
            "uge":    "无符号整数大于等于比较",
            # ---- 导数（像素着色器）----
            "deriv_rtx":       "屏幕 X 方向导数（ddx）",
            "deriv_rty":       "屏幕 Y 方向导数（ddy）",
            "deriv_rtx_coarse":"ddx 粗略版（2x2 quad）",
            "deriv_rty_coarse":"ddy 粗略版（2x2 quad）",
            "deriv_rtx_fine":  "ddx 精细版",
            "deriv_rty_fine":  "ddy 精细版",
            # ---- 纹理采样 / 资源读取 ----
            "sample":           "纹理采样",
            "sample_l":         "指定 LOD 采样",
            "sample_b":         "带 LOD 偏移的采样",
            "sample_d":         "指定梯度的采样（各向异性）",
            "sample_c":         "比较采样（阴影 PCF）",
            "sample_c_lz":      "比较采样 LOD=0（点阴影）",
            "gather4":          "四点采样（返回 2×2 邻域）",
            "gather4_c":        "四点比较采样",
            "gather4_po":       "四点采样（可编程偏移）",
            "gather4_po_c":     "四点比较采样（可编程偏移）",
            "ld":               "无采样器读取（Load，按整数坐标）",
            "ld_ms":            "MSAA 纹理读取",
            "ld_raw":           "ByteAddressBuffer 原始读取",
            "ld_structured":    "StructuredBuffer 读取",
            "ld_uav_typed":     "从类型化 UAV 读取",
            "lod":              "查询 LOD 计算结果",
            "resinfo":          "查询资源尺寸/mip 数",
            "bufinfo":          "查询 buffer 元素数量",
            "check_access_fully_mapped":"稀疏资源访问检查",
            # ---- UAV / 存储 ----
            "store_raw":        "ByteAddressBuffer 写入",
            "store_structured": "StructuredBuffer 写入",
            "store_uav_typed":  "类型化 UAV 写入",
            # ---- 原子操作 ----
            "atomic_and":       "原子按位与",
            "atomic_or":        "原子按位或",
            "atomic_xor":       "原子按位异或",
            "atomic_iadd":      "原子整数加",
            "atomic_imax":      "原子有符号取大",
            "atomic_imin":      "原子有符号取小",
            "atomic_umax":      "原子无符号取大",
            "atomic_umin":      "原子无符号取小",
            "atomic_cmp_store": "原子比较写入",
            "imm_atomic_alloc": "追加缓冲区分配位置",
            "imm_atomic_consume":"消费缓冲区取位置",
            "imm_atomic_iadd":  "即时原子加（返回旧值）",
            "imm_atomic_and":   "即时原子与",
            "imm_atomic_or":    "即时原子或",
            "imm_atomic_xor":   "即时原子异或",
            "imm_atomic_exch":  "即时原子交换",
            "imm_atomic_cmp_exch":"即时原子比较交换",
            # ---- 控制流 ----
            "if":       "分支开始（if 条件成立进入）",
            "else":     "分支 else 分支",
            "endif":    "分支结束",
            "loop":     "循环开始",
            "endloop":  "循环结束",
            "break":    "跳出循环",
            "breakc":   "条件跳出循环",
            "continue": "继续下一轮循环",
            "continuec":"条件 continue",
            "switch":   "switch 开始",
            "case":     "case 分支",
            "default":  "default 分支",
            "endswitch":"switch 结束",
            "ret":      "函数返回",
            "retc":     "条件返回",
            "call":     "函数调用",
            "callc":    "条件函数调用",
            "label":    "标签",
            "discard":  "丢弃像素（无条件）",
            "discard_nz":"条件丢弃像素（非零则丢弃）",
            "discard_z": "条件丢弃像素（为零则丢弃）",
            "nop":      "空操作",
            # ---- 同步（Compute Shader）----
            "sync":     "线程组同步屏障",
            "emit":     "GS 发射顶点",
            "cut":      "GS 结束条带",
            "emit_stream":"GS 指定流发射",
            "cut_stream": "GS 指定流结束",
            "emitthencut":"GS 发射并结束",
            # ---- Hull/Domain Shader ----
            "hs_control_point_phase":"HS 控制点阶段",
            "hs_fork_phase":         "HS fork 阶段",
            "hs_join_phase":         "HS join 阶段",
            # ---- 特殊 ----
            "abs":      "取绝对值（源修饰符）",
            "neg":      "取负（源修饰符）",
            "saturate": "饱和到 [0,1]（目标修饰符）",
        }

        # 后缀说明表（用于组合出更完整的解释）
        SUFFIX_HELP = {
            "sat":        "结果饱和到 [0,1]",
            "indexable":  "可索引资源版本",
            "aoffimmi":   "带立即数偏移",
        }

        def lookup_op(op):
            """返回 opcode 的中文说明。处理 _sat / _indexable 等后缀。"""
            if op in OP_HELP:
                return OP_HELP[op]
            # 逐段剥离后缀查表
            parts = op.split("_")
            suffixes = []
            while len(parts) > 1:
                tail = parts[-1]
                if tail in SUFFIX_HELP:
                    suffixes.insert(0, SUFFIX_HELP[tail])
                    parts.pop()
                    base = "_".join(parts)
                    if base in OP_HELP:
                        return OP_HELP[base] + "（" + "，".join(suffixes) + "）"
                else:
                    break
            # 前缀匹配兜底
            for key, val in OP_HELP.items():
                if op.startswith(key + "_"):
                    return val + "（变体）"
            return "未收录指令，请查阅 DXBC 文档"

        lines = []
        slots = s["slots"]
        cats = s["categories"]
        ops = s["opcodes"]
        total_exec = sum(v for k, v in cats.items() if k != "decl")

        lines.append("========== 核心指标 ==========")
        lines.append(f"Instruction Slots（指令槽数，fxc 官方报告）: {slots}")
        lines.append("  → D3D 虚拟 ISA 的指令槽数，评估 shader 复杂度的主要参考。")
        lines.append("  → 部分复合指令占多槽（如 sincos=2），纹理采样只算 1 槽但实际很贵。")
        lines.append("")
        lines.append(f"Counted Opcodes（本工具计数的可执行指令数）: {total_exec}")
        lines.append("  → 逐行统计的 opcode 数量，已排除声明与注释，供分类分析。")
        lines.append("  → 与官方槽数通常略有差异，属正常现象。")
        lines.append("")
        lines.append("========== 按类别统计 ==========")
        for k in ("alu", "memory", "control", "other", "decl"):
            v = cats.get(k, 0)
            pct = (v * 100.0 / total_exec) if (total_exec and k != "decl") else 0
            label = CAT_LABELS.get(k, k)
            if k == "decl":
                lines.append(f"  [{v:4d}]  {label}")
            else:
                lines.append(f"  [{v:4d}] ({pct:5.1f}%)  {label}")
        lines.append("")
        lines.append("========== Opcode 使用频次 Top 25 ==========")
        lines.append(f"  {'指令':22s} {'次数':>6s}  说明")
        lines.append(f"  {'-'*22} {'-'*6}  {'-'*40}")
        for op, n in ops.most_common(25):
            help_txt = lookup_op(op)
            lines.append(f"  {op:22s} {n:6d}  {help_txt}")
        return "\n".join(lines)

    # ---- helpers ----
    def _set_ro(self, widget, text):
        widget.configure(state=tk.NORMAL)
        widget.delete("1.0", tk.END)
        widget.insert("1.0", text)
        widget.configure(state=tk.DISABLED)

    def log(self, msg):
        if not msg:
            return
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, msg)
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)


if __name__ == "__main__":
    App().mainloop()
