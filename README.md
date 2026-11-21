# Porism

Porism 是一种以表达式求值为核心、按语句序列顺序执行的解释型语言。求值内核以 C 语言实现，并通过 `porism.h` 定义统一 C ABI；同一份源码既构建命令行入口，也构建供宿主链接的动态库与静态库。

- 求值内核仅依赖 C 标准库与 libm，可由 C11 编译器构建。
- 数值以 `double` 为主载体；带 `exact` 标记的数值同时保存 `__int128` 精确整数，超出 `I128_SAFE` 时退回双精度浮点表示。
- 结果对象模式输出 JSON 结果对象；诊断包含错误类别及 1 起算的行号、列号。
- 并发调用在全局互斥下串行化；错误与输出状态保存于线程本地调用上下文。

## 构建

在 Windows 平台，应使用 MSYS2 的 MINGW64 / UCRT64 / CLANG64 终端构建；MSVC 不受支持（见「实现约束与限制」）。Linux 平台可直接构建。

```bash
cd core
./run.sh build                              # -> dist/<platform>-<arch>/{bin,lib}
```

| 目标产物 | Windows | Linux |
| --- | --- | --- |
| 命令行入口 | `bin/porism.exe` | `bin/porism.linux`（包内命名为 `porism`） |
| 动态库 | `bin/porism.dll` | `lib/libporism.so.<版本>` |
| 导入库 | `lib/porism.lib`(MSVC)、`libporism.dll.a`(GNU)、`porism.def` | — |
| 静态库 | `lib/libporism.a` | `lib/libporism.a` |
| 头文件 | `include/porism.h` | `include/porism.h` |

`./run.sh help` 列出全部配置项：`PORISM_VERSION`、`PORISM_SOVER`、`PORISM_WERROR`、`PORISM_DIST`、`PORISM_7Z`、`CC`、`AR`。

## 命令行入口

> **参数引用规则**：各 Shell 对引号的处理不同；引号形式不当时，传入的源码文本会发生改变。
> - `cmd.exe` 不将单引号识别为引用符：`porism 'Output(1)'` 会把单引号一并传入，源码被解析为字符串字面量，产生 `status` 为 `ok`、`outputs` 为空的结果对象。
> - PowerShell 5.1 会剥离参数中的裸双引号：`-p '{"x":"41"}'` 不构成合法参数对象，须写成 `-p '{\"x\":\"41\"}'`。
> - bash / MSYS2 / Linux / macOS 可用单引号包裹参数对象。

```bash
# bash / MSYS2 / Linux
porism "Output(1+2*3)"                             # -> 7
porism 'Output(num($x) + 1)' -p '{"x":"41"}'       # -> 42
porism 'Output(num($x) + 1)' -P params.json        # -> 42（参数对象从文件读取）
porism --json 'Output(num($x) + 1)' -p '{"x":"41"}'# -> {"status":"ok","errors":null,"outputs":["42"]}
porism -f demo.prsm
porism --version
```

```powershell
# PowerShell 5.1
.\porism.exe 'Output(num($x) + 1)' -p '{\"x\":\"41\"}'
```

```bat
REM Windows CMD
porism.exe "Output(num($x) + 1)" -p "{\"x\":\"41\"}"
```

命令行入口默认以文本输出模式运行，向标准输出逐行写入 `outputs` 的内容；诊断写入标准错误，不混入输出。加 `--json`（或 `-j`）后，改为向标准输出写入一行完整结果对象：成功为 `{"status":"ok","errors":null,"outputs":[...]}`，失败为 `{"status":"error","errors":[{"id","type","message","line","col"}],"outputs":[...]}`。错误发生前已产生的输出予以保留，不因后续错误而清空。错误类别取值 `code` / `kernel` / `parameter` / `unknown`。退出码为 `0`（成功）或 `1`（错误）。

C ABI 的结果对象语义不受该参数影响：`porism_run_json` 始终返回结果对象；`--json` 仅决定命令行入口把哪一种形式写到标准输出。

## 语言概要

```porism
# 注释
x = 2 ** 100                       # 精确整数，无精度损失
f(a, b) = a * a + b               # 函数定义，函数体是单个表达式
Output(f(3, 4))                    # -> 13
Output("a" * 3)                    # -> aaa
```

`Output` 为输出语句，仅可出现在顶层，且实参个数须为 1；出现在表达式位置或传入多个实参时产生相应诊断。

- 运算符：`+ - * / // % ** == != < <= > >= && || ! ?:`
- 字符串字面量以单引号或双引号定界，允许 `\n \t \r \\ \" \'` 转义；`\0` 转义被拒绝并产生诊断，字符串值以 NUL 结尾的字节序列表示。
- 内置常量：`true` `false` `null` `pi` `e`；上述名称为名称查找阶段的内置绑定，不得作为赋值目标。
- 内建函数：`abs sqrt pow min max round num float str len upper lower`
- 命令行入口以 `-p "<json>"` 注入内联参数对象，或以 `-P <params.json>` 从文件读取参数对象；两者互斥。参数对象为平铺 JSON 对象，值须为 JSON 字符串。源码中的参数引用 `$KEY` 求值为对应键的字符串值，参与数值运算前须经 `num()` 或 `float()` 显式转换。

## C ABI

`core/porism.h` 定义统一 C ABI；`PORISM_ABI_VERSION` 为 `0.1.0-Beta`。

| 接口 | 行为 |
| --- | --- |
| `porism_run(src, out, cap)` | 执行源码；不注入参数对象；文本输出模式 |
| `porism_eval(expr, names, vals, nvars, out, cap)` | 求值单个表达式并注入宿主变量；文本输出模式；不注入参数对象 |
| `porism_run_json(src, params_json, out, cap)` | 执行源码、注入参数对象并返回结果对象；仅该接口支持参数引用 `$KEY` |
| `porism_error*` / `porism_truncated` | 状态访问器：查询错误与截断状态 |

返回码：`0` 成功；`1` 语言、参数或内核错误；`2` 输出缓冲区或结果对象缓冲区不足。结果对象模式下缓冲区不足时，以 `kernel` 类信息填充错误区、将输出缓冲区置为空串。状态访问器读取线程本地调用上下文，须与执行接口在同一线程调用。

## 实现约束与限制

- **编译器**：求值内核使用 `__int128` 与变长数组，MSVC 不予支持；应使用 GCC、Clang 或 MinGW-w64 编译。Windows 发布产物由 MinGW-w64 构建，`porism.lib` 为供 MSVC 链接使用的导入库。
- **区域设置**：数值字面量由 `strtod` 扫描，值的文本格式化使用 `snprintf` 的 `%g` / `%.*f`，均受 `LC_NUMERIC` 影响。命令行入口独立运行时采用 C locale；作为库嵌入且宿主将 `LC_NUMERIC` 设置为逗号小数点区域时，`1.5` 会被错误切分。
- **非 ASCII 大小写**：`upper` / `lower` 逐字节调用 `toupper` / `tolower`，不执行 Unicode 转换；`len` 按 UTF-8 码点计数。
- **Windows 控制台编码**：源码应为 NUL 结尾的 UTF-8 字符串；非 ASCII 源码建议以 `-f` 从文件读取，或先执行 `chcp 65001`。经命令行参数直接传入时可能被本地代码页转换。
- **输出缓冲区**：命令行入口的结果缓冲区从 1 MiB 起按需扩容（上限 1 GiB）；宿主调用须自行提供足够的 `out` / `cap`。
- **诊断文本**：字符串值以 NUL 结尾，不接受内嵌 NUL；定义 `PORISM_ZH` 时主要诊断文本切换为中文，部分以字符串字面量直接给出的诊断固定为英文。

## 许可证

Apache License 2.0，见 [LICENSE](LICENSE)。
