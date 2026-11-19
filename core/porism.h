/**
* Porism 求值内核的 C ABI
*/

// 虽然但是，这个Header的注释真的好丑

#ifndef PORISM_H
#define PORISM_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// The version of Porism.
#ifndef PORISM_VERSION
#define PORISM_VERSION "1.0.0"       /* 可由构建脚本 -DPORISM_VERSION=... 覆盖 */
#endif
#if defined(_WIN32) && defined(PORISM_DLL)
#  define PORISM_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PORISM_DLL_USE)
#  define PORISM_API __declspec(dllimport)
#else
#  define PORISM_API
#endif

/**
* 执行一段 Porism 源码
* @param src NUL 结尾的 UTF-8 源码
* @param out 输出缓冲区；传 NULL 时结果直接打到 stdout
* @param cap 缓冲区字节数
* @return `0`: 成功；`1`: 语言错误；`2`: 输出被截断。
* 每次调用都会重置，不保留跨调用状态。
*/
PORISM_API int porism_run(const char *src, char *out, size_t cap);

/**
* 求值单个表达式，并注入宿主变量
* @param expr  表达式源码
* @param names 变量名数组
* @param vals  与`names`对应的`double`值
* @param nvars 变量个数
* @param out   输出缓冲区；传 NULL 时结果直接打到 stdout
* @param cap   out 的字节数
* @return  返回值与 porism_run 相同。变量以 double 注入，报错行列对应 expr 本身。
*/

PORISM_API int porism_eval(const char *expr, const char **names, const double *vals,
                           int nvars, char *out, size_t cap);

/**
* 执行一次求值任务并返回结果对象
* @param src         NUL 结尾的 UTF-8 源码
* @param params_json -p 的平铺 JSON 对象字符串
* @param out         结果 JSON 缓冲区；传 NULL 时直接打印到 stdout
* @param cap         缓冲区字节数
*
* 成功时 out 为`{"status": "ok", "errors": null, "outputs": [...]}`
*
* 出错时为`{"status": "error", "errors": [{"id","type","message","line","col"}, ...], "outputs": [...]}`
*
* type 取值 `code` / `kernel` / `parameter` / `unknown`
* 出错前已产生的 `outputs` 会保留。
*
* params_json 必须是平铺对象，值只能是字符串。`$key` 求值为字符串，
* 需要数值时用 `num($key)` 显式转换。UTF-16 代理对会合并为单个码点。
*
* @return  0 = status ok  1 = status error  2 = 结果缓冲区不足
*/
PORISM_API int porism_run_json(const char *src, const char *params_json,
                               char *out, size_t cap);


PORISM_API const char   *porism_error(void);                /* 错误信息，仅在返回 1 后有效          */
PORISM_API const char   *porism_error_type(void);           /* code / kernel / parameter / unknown  */
PORISM_API int          porism_error_line(void);            /* 1-based 行号，0 表示无位置           */
PORISM_API int          porism_error_col(void);             /* 1-based 列号                         */
PORISM_API size_t       porism_output_len(void);            /* 实际写入的字节数（不含结尾 NUL）     */
PORISM_API int          porism_truncated(void);             /* 结果是否被截断                       */

#ifdef __cplusplus
}
#endif
#endif /* PORISM_H */