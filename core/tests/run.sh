#!/usr/bin/env bash
# Porism 回归测试
# 用法: ./tests/run.sh [porism 可执行文件路径]
# 不传路径时自动在 core/dist/ 下查找；建议发布前执行 ./run.sh release 再跑本脚本。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -n "${1:-}" ]; then
  BIN="$1"
else
  BIN=""
  for c in \
    "$ROOT/dist/windows-x86_64/bin/porism.exe" \
    "$ROOT/dist/linux-x86_64/bin/porism.linux" \
    "$ROOT/dist/linux-aarch64/bin/porism.linux" \
    "$ROOT/porism"; do
    if [ -x "$c" ]; then BIN="$c"; break; fi
  done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "用法: $0 <porism 可执行文件路径>（或先执行 core/run.sh build）" >&2
  exit 2
fi

pass=0
fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL %s\n       %s\n' "$1" "$2"; fail=$((fail + 1)); }

# check <名称> <期望返回码> <必须包含的字串> <porism 参数...>
check() {
  local name="$1" want_rc="$2" needle="$3"
  shift 3
  local out rc
  out="$("$BIN" "$@" 2>/dev/null)"
  rc=$?
  if [ "$rc" -ne "$want_rc" ]; then
    bad "$name" "返回码 $rc，期望 $want_rc；输出: $out"
    return
  fi
  if [ -n "$needle" ] && ! printf '%s' "$out" | grep -Fq -- "$needle"; then
    bad "$name" "输出不含 '$needle'；实际: $out"
    return
  fi
  ok "$name"
}

TMP="$(mktemp -d)"
trap 'rm -f "$TMP"/* 2>/dev/null; rmdir "$TMP" 2>/dev/null || true' EXIT

printf '使用: %s\n\n' "$BIN"

echo "[表达式与内置函数]"
check "算术优先级"         0 '"outputs":["7"]'                              'Output(1+2*3)'
check "精确整数幂"         0 '"outputs":["1267650600228229401496703205376"]' 'Output(2**100)'
check "round 银行家舍入"   0 '"outputs":["2"]'                              'Output(round(2.5))'
check "字符串拼接"         0 '"outputs":["Hello, World"]'                   'Output("Hello, " + "World")'
check "自定义函数"         0 '"outputs":["49"]'                             'f(x)=x*x; Output(f(7))'
check "三元与布尔"         0 '"outputs":["true"]'                           'Output(1 < 2 && !false)'
check "字符串长度"         0 '"outputs":["3"]'                              'Output(len("abc"))'
check "JSON 转义"          0 '"outputs":["a\"b"]'                           'Output("a\"b")'

echo "[错误分类与消息]"
check "pow 参数个数"       1 'function pow needs 2 args, got 1'              'pow(1)'
check "round 参数个数"     1 'function round needs 1-2 args, got 3'         'round(1,2,3)'
check "min 参数个数"       1 'function min needs at least 1 args, got 0'    'min()'
check "未定义变量"         1 'undefined variable'                            'Output(nope)'
check "除以零"             1 'division by zero'                              'Output(1/0)'
check "内嵌 NUL 被拒绝"    1 'embedded NUL'                                  'Output("a\0b")'
check "出错前输出保留"     1 '"outputs":["first"]'                           $'Output("first")\nOutput(nope)'

echo "[解析深度：必须返回错误而不是崩溃]"
DEEP_OPEN="$(printf '%*s' 3000 '' | tr ' ' '(')"
DEEP_CLOSE="$(printf '%*s' 3000 '' | tr ' ' ')')"
DEEP_NEG="$(printf '%*s' 3000 '' | tr ' ' '-')"
POW_CHAIN="1$(printf '**1%.0s' {1..3000})"
check "3000 层括号"        1 'expression nesting too deep'                   "Output(${DEEP_OPEN}1${DEEP_CLOSE})"
check "3000 层一元运算符"  1 'expression nesting too deep'                   "Output(${DEEP_NEG}1)"
check "3000 层幂运算"      1 'expression nesting too deep'                   "Output(${POW_CHAIN})"

echo "[参数、文件与 CLI]"
check "-p 平铺参数"        0 '"outputs":["42"]'                             'Output(num($x) + 1)' -p '{"x":"41"}'
check "-p 缺失参数"        1 'parameter $y not provided by -p'              'Output($y)'
printf 'Output(len("你好"))' > "$TMP/utf8.prsm"
check "-f UTF-8 源文件"    0 '"outputs":["2"]'                              -f "$TMP/utf8.prsm"
check "--version"          0 'Porism Interpreter v'                         --version
check "--help"             0 'Usage:'                                       --help

echo "[大结果：不得静默丢弃]"
{ printf 'Output("'; printf '%*s' 1200000 '' | tr ' ' 'a'; printf '")'; } > "$TMP/big.prsm"
"$BIN" -f "$TMP/big.prsm" > "$TMP/big.out" 2>/dev/null
rc=$?
bytes="$(wc -c < "$TMP/big.out" | tr -d ' ')"
if [ "$rc" -eq 0 ] && [ "$bytes" -gt 1200000 ]; then
  ok "1.2MB 结果完整输出（${bytes} 字节）"
else
  bad "1.2MB 结果完整输出" "返回码 $rc，实际输出 $bytes 字节"
fi

echo
printf '通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
