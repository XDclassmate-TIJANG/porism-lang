#!/usr/bin/env bash
# porism 构建 / 打包（MSYS2 MinGW / Linux）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

log()  { printf '[%s] %s\n' "$1" "$2"; }
step() { printf '[package] %s\n' "$*"; }
warn() { printf '[警告] %s\n' "$*" >&2; }
die()  { printf '[错误] %s\n' "$*" >&2; exit 1; }
cfg()  { local v="${!1-}"; if [[ -n "$v" ]]; then printf '%s' "$v"; else printf '%s' "$2"; fi; }

declare -a ARGS=()

# 允许 KEY=VALUE / -KEY=VALUE 出现在任意位置
parse_args() {
  local -a pos=()
  local a k
  for a in "$@"; do
    if [[ "$a" =~ ^-{0,2}([A-Z][A-Z0-9_]*)=(.*)$ ]]; then
      k="${BASH_REMATCH[1]}"
      case "$k" in
        PORISM_VERSION|PORISM_SOVER|PORISM_WERROR|PORISM_DIST|PORISM_7Z|CC|AR)
          export "$k=${BASH_REMATCH[2]}" ;;
        *) die "未知参数：$a" ;;
      esac
    else
      pos+=("$a")
    fi
  done
  ARGS=()
  if ((${#pos[@]})); then ARGS=("${pos[@]}"); fi
}

load_cfg() {
  VERSION="$(cfg PORISM_VERSION 0.1.0-Beta)"
  SOVER="$(cfg PORISM_SOVER 1)"
  CC="$(cfg CC gcc)"
  AR="$(cfg AR ar)"
  WERROR="$(cfg PORISM_WERROR 0)"
  TMP_PARENT="${TMPDIR:-/tmp}"
}

usage() {
  cat <<'EOF'
用法: ./run.sh <命令> [release|debug] [-KEY=VALUE ...]

命令:
  build     构建到 dist/<platform>-<arch>/{bin,lib}
  package   把已构建产物整理成发布包
  release   build + package
  clean     删除输出目录
  help      本帮助

配置（环境变量，或 -KEY=VALUE / KEY=VALUE，位置不限）:
  PORISM_VERSION  版本号，默认 1.0.0（写进 --version 与包名）
  PORISM_SOVER    Linux SONAME 主版本，默认 1
  PORISM_WERROR   1 = 追加 -Werror
  PORISM_DIST     输出目录，默认 core/dist
  PORISM_7Z       7z 可执行文件路径；默认在 PATH / Program Files 中查找
  CC / AR         编译器与归档器，默认 gcc / ar

示例:
  ./run.sh build
  ./run.sh release -PORISM_VERSION=1.0.0 -PORISM_WERROR=1

约定:
  Windows 输出 zip（用 7-Zip 或 zip 命令），Linux 输出 tar.gz；
  包内平铺 bin/ lib/ include/ 与 LICENSE、README.md，另生成同名 .sha256。
  源码 porism.c / 头文件在脚本同目录，LICENSE / README.md 在仓库根目录。
EOF
}

# ---------- 平台 ----------
detect_platform() {
  local u m d
  u="$(uname -s)"
  m="$(uname -m)"
  case "$u" in
    MINGW*_NT-*) PLATFORM=windows ;;
    Linux)       PLATFORM=linux ;;
    CYGWIN*|MSYS*|Windows_NT)
      die "$u 是 POSIX 仿真环境：请用 MSYS2 的 UCRT64 / MINGW64 / CLANG64 终端构建" ;;
    *) die "不支持的平台：$u" ;;
  esac
  case "$m" in
    x86_64|amd64)        ARCH=x86_64 ;;
    aarch64|arm64)       ARCH=aarch64 ;;
    i386|i486|i586|i686) ARCH=x86 ;;
    armv7*)              ARCH=armv7 ;;
    *)                   ARCH="$m" ;;
  esac

  d="$(cfg PORISM_DIST "$SCRIPT_DIR/dist")"
  mkdir -p -- "$d" || die "无法创建输出目录：$d"
  DIST="$(cd -- "$d" && pwd -P)"
  OUT="$DIST/${PLATFORM}-${ARCH}"
}

setup_flags() {
  CFG="$1"
  case "$CFG" in release|debug) ;; *) die "未知配置：$CFG（release|debug）" ;; esac

  CFLAGS=(-std=c11 -Wall -Wextra)
  if [[ "$CFG" == release ]]; then
    CFLAGS+=(-Os); LINK_FLAGS=(-s); CFG_SUFFIX=""
  else
    CFLAGS+=(-O0 -g); LINK_FLAGS=(); CFG_SUFFIX="-debug"
  fi
  if [[ "$WERROR" == 1 ]]; then CFLAGS+=(-Werror); fi
  # 让 --version 与包版本号一致（porism.h 有 #ifndef 守卫，可安全覆盖）
  CFLAGS+=("-DPORISM_VERSION=\"$VERSION\"")

  case "$PLATFORM" in
    windows)
      EXE=porism.exe; DLL=porism.dll; STATIC=libporism.a; OBJ=porism.o
      LINK_FLAGS+=(-static-libgcc) ;;
    linux)
      EXE=porism.linux; DLL="libporism.so.$VERSION"
      STATIC=libporism_linux.a; OBJ=porism_linux.o
      CFLAGS+=(-fPIC -pthread); LINK_FLAGS+=(-pthread) ;;
  esac
  LIBS=(-lm "${LINK_FLAGS[@]}")
}

# Windows 打包器：本机通常没有 zip 命令，优先用 7-Zip 生成 zip
find_packer() {
  local p c
  p="$(cfg PORISM_7Z '')"
  if [[ -n "$p" ]]; then
    [[ -x "$p" ]] || die "PORISM_7Z 指向的文件不可执行：$p（请用 MSYS2 路径，如 /i/7-Zip/7z.exe）"
    printf '%s\n' "$p"
    return 0
  fi
  for c in 7z 7zz 7za "/c/Program Files/7-Zip/7z.exe" "/c/Program Files (x86)/7-Zip/7z.exe"; do
    if [[ "$c" == */* ]]; then
      if [[ -x "$c" ]]; then printf '%s\n' "$c"; return 0; fi
    elif command -v "$c" >/dev/null 2>&1; then
      printf '%s\n' "$c"; return 0
    fi
  done
  if command -v zip >/dev/null 2>&1; then printf '%s\n' zip; return 0; fi
  return 1
}

# ---------- 构建 ----------
# 精确清理由本脚本生成的旧产物，不递归删除 $OUT：
#  1) 自定义 PORISM_DIST 里可能有其它文件，rm -rf 整个目录会误删；
#  2) 部分终端把 rm -rf 重定向到回收站并 fail-closed，删目录失败会中断构建。
clean_out() {
  local f
  local -a olds=(
    "$OUT/bin/$EXE" "$OUT/bin/$DLL" "$OUT/bin/porism" "$OUT/bin/porism.linux"
    "$OUT/lib/$STATIC" "$OUT/lib/libporism.a" "$OUT/lib/libporism_linux.a"
    "$OUT/lib/libporism.dll.a" "$OUT/lib/porism.def"
    "$OUT/lib/porism.lib" "$OUT/lib/porism.exp"
  )
  # 版本化 .so / SONAME 软链：VERSION 或 SOVER 变化后会残留
  for f in "$OUT"/lib/libporism.so.*; do
    [[ -e "$f" || -L "$f" ]] && olds+=("$f")
  done
  for f in "${olds[@]}"; do
    if [[ -e "$f" || -L "$f" ]]; then rm -f -- "$f" || true; fi
  done
}

# 删除打包暂存目录；用 find -delete 而非 rm -rf，规避终端删除保护
cleanup_stage() {
  local d="$1"
  [[ -d "$d" ]] || return 0
  find "$d" -depth -delete >/dev/null 2>&1 || true
}

# GNU ld 的 .dll.a 不是 MSVC 原生导入库；用同次链接产出的 .def 重建
build_msvc_implib() {
  local def="$OUT/lib/porism.def" out="$OUT/lib/porism.lib" m lm wdef wout
  case "$ARCH" in
    x86_64)  m=X64;   lm="i386:x86-64" ;;
    x86)     m=X86;   lm="i386" ;;
    aarch64) m=ARM64; lm="aarch64" ;;
    *)       m=ARM;   lm="arm" ;;
  esac
  [[ -s "$def" ]] || return 1

  if command -v llvm-dlltool >/dev/null 2>&1 &&
     llvm-dlltool -m "$lm" -d "$def" -D "$DLL" -l "$out" >/dev/null 2>&1; then return 0; fi
  if command -v lib.exe >/dev/null 2>&1; then
    wdef="$def"; wout="$out"
    if command -v cygpath >/dev/null 2>&1; then wdef="$(cygpath -w "$def")"; wout="$(cygpath -w "$out")"; fi
    if MSYS2_ARG_CONV_EXCL='*' lib.exe /NOLOGO "/DEF:$wdef" "/MACHINE:$m" "/OUT:$wout" >/dev/null 2>&1; then return 0; fi
  fi
  if command -v dlltool >/dev/null 2>&1 && dlltool -d "$def" -D "$DLL" -l "$out" >/dev/null 2>&1; then return 0; fi
  return 1
}

do_build() {
  local cfg="${1:-release}" obj
  local src="$SCRIPT_DIR/porism.c"
  detect_platform
  setup_flags "$cfg"

  [[ -f "$src" ]] || die "找不到 $src"
  command -v "$CC" >/dev/null 2>&1 || die "找不到编译器 $CC"
  command -v "$AR" >/dev/null 2>&1 || die "找不到归档器 $AR"

  log "build" "$PLATFORM-$ARCH $CFG $VERSION -> $OUT"
  clean_out
  mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/.obj"
  obj="$OUT/.obj/$OBJ"

  "$CC" "${CFLAGS[@]}" "$src" -o "$OUT/bin/$EXE" "${LIBS[@]}"

  if [[ "$PLATFORM" == windows ]]; then
    # PORISM_LIB 去掉 main；PORISM_DLL 让导出表只有 PORISM_API 标记的函数
    "$CC" "${CFLAGS[@]}" -shared -DPORISM_LIB -DPORISM_DLL "$src" \
      -o "$OUT/bin/$DLL" "${LIBS[@]}" \
      -Wl,--out-implib,"$OUT/lib/libporism.dll.a" \
      -Wl,--output-def,"$OUT/lib/porism.def" -Wl,--no-undefined
    if ! build_msvc_implib; then
      cp -a "$OUT/lib/libporism.dll.a" "$OUT/lib/porism.lib"
      warn "porism.lib 为 .dll.a 副本（MSVC 多库链接可能异常）；建议提供 llvm-dlltool 或 lib.exe"
    fi
  else
    # 只产实体 .so；软链延后到暂存目录建立（WSL 的 /mnt/* 默认禁止 ln -s）
    "$CC" "${CFLAGS[@]}" -shared -DPORISM_LIB "$src" \
      -o "$OUT/lib/$DLL" "${LIBS[@]}" \
      "-Wl,-soname,libporism.so.$SOVER" -Wl,--no-undefined
  fi

  "$CC" "${CFLAGS[@]}" -c -DPORISM_LIB "$src" -o "$obj"
  "$AR" rcs "$OUT/lib/$STATIC" "$obj"
  rm -f -- "$obj" || true
  rmdir "$OUT/.obj" >/dev/null 2>&1 || true
}

# ---------- 打包 ----------
do_package() {
  local cfg="${1:-release}" stage stage_parent pkgname archive packer archive_arg zip_arg
  local f hr ext hash
  detect_platform
  setup_flags "$cfg"

  [[ -d "$OUT/bin" && -d "$OUT/lib" ]] || die "尚未构建：$OUT"
  if [[ "$PLATFORM" == windows ]]; then
    packer="$(find_packer || true)"
    [[ -n "$packer" ]] || die "缺打包器：把 7-Zip 加入 PATH，或用 -PORISM_7Z=/i/7-Zip/7z.exe 指定；也可安装 zip 命令"
  fi

  pkgname="porism-${VERSION}-${PLATFORM}-${ARCH}${CFG_SUFFIX}"
  stage_parent="$(mktemp -d "$TMP_PARENT/porism-stage.XXXXXX")" \
    || die "无法创建暂存目录：$TMP_PARENT"
  stage="$stage_parent/$pkgname"
  mkdir -p "$stage/bin" "$stage/lib" "$stage/include"

  step "1/3 整理产物与文档"
  cp -a "$OUT/lib/." "$stage/lib/"
  # lib.exe 的中间产物不进包；暂存目录在 /tmp，用 find -delete 规避删除保护
  find "$stage/lib" -maxdepth 1 -type f -name 'porism.exp' -delete >/dev/null 2>&1 || true
  if [[ "$PLATFORM" == windows ]]; then
    cp -a "$OUT/bin/$EXE" "$OUT/bin/$DLL" "$stage/bin/"
    for f in porism.lib porism.def libporism.dll.a libporism.a; do
      [[ -f "$stage/lib/$f" ]] || warn "包内缺少 lib/$f"
    done
  else
    install -m 755 "$OUT/bin/$EXE" "$stage/bin/porism"
    if [[ -f "$stage/lib/$STATIC" ]]; then mv "$stage/lib/$STATIC" "$stage/lib/libporism.a"; fi
    ln -sfn "$DLL" "$stage/lib/libporism.so.$SOVER" 2>/dev/null \
      || cp -a "$stage/lib/$DLL" "$stage/lib/libporism.so.$SOVER"
    ln -sfn "libporism.so.$SOVER" "$stage/lib/libporism.so" 2>/dev/null \
      || cp -a "$stage/lib/$DLL" "$stage/lib/libporism.so"
  fi

  hr="$SCRIPT_DIR/porism.h"
  if [[ -d "$SCRIPT_DIR/include" ]]; then hr="$SCRIPT_DIR/include"; fi
  if [[ -d "$hr" ]]; then
    cp -a "$hr/." "$stage/include/"
  elif [[ -f "$hr" ]]; then
    cp -a "$hr" "$stage/include/"
  else
    warn "未找到 porism.h：SDK 用户无法链接"
  fi
  for f in LICENSE README.md CHANGELOG.md; do
    if [[ -f "$ROOT_DIR/$f" ]]; then cp -a "$ROOT_DIR/$f" "$stage/"; fi
  done
  [[ -f "$stage/LICENSE" ]] || warn "未找到 LICENSE"

  if [[ "$PLATFORM" == windows ]]; then ext=zip; else ext=tar.gz; fi
  archive="$DIST/${pkgname}.${ext}"
  # 7z a / zip -r 是增量更新，同名旧包会残留本次已删除的条目；先清掉
  find "$DIST" -maxdepth 1 -type f -name "${archive##*/}" -delete >/dev/null 2>&1 || true

  step "2/3 压缩 -> ${archive##*/}"
  if [[ "$PLATFORM" == windows ]]; then
    if [[ "$(basename "$packer")" == zip || "$(basename "$packer")" == zip.exe ]]; then
      zip_arg="$archive"
      if [[ "$packer" == *.exe ]] && command -v cygpath >/dev/null 2>&1; then
        zip_arg="$(cygpath -w "$archive")"
      fi
      ( cd "$stage" && "$packer" -qry "$zip_arg" . )
    else
      # command -v 7z 在 MSYS2 上可能返回不带 .exe 的路径，按平台统一转 Windows 路径
      archive_arg="$archive"
      if command -v cygpath >/dev/null 2>&1; then archive_arg="$(cygpath -w "$archive")"; fi
      ( cd "$stage" && "$packer" a -tzip -mx=9 -y -- "$archive_arg" . >/dev/null )
    fi
  else
    ( cd "$stage" && find . -mindepth 1 -maxdepth 1 -print0 | sort -z \
        | tar --null -czf "$archive" -T - )
  fi

  # 校验压缩包本身：用户下载后与 .sha256 放同目录，sha256sum -c 即可
  ( cd "$DIST" && sha256sum "${archive##*/}" > "${archive##*/}.sha256" )
  hash="$(cut -d' ' -f1 < "${archive}.sha256")"
  step "3/3 校验和 ${archive##*/}.sha256  $hash"
  cleanup_stage "$stage_parent"
}

do_clean() {
  local d
  d="$(cfg PORISM_DIST "$SCRIPT_DIR/dist")"
  # 防止误传盘根 / 家目录 / 脚本根目录导致不可逆删除
  case "$d" in
    /|[A-Za-z]:/|/c|/c/|/mnt/?|/mnt/?/|/tmp|/tmp/|/var/tmp|"$HOME"|"$SCRIPT_DIR")
      die "拒绝清理危险目录：$d" ;;
  esac
  if [[ -d "$d" ]]; then
    find "$d" -depth -delete >/dev/null 2>&1 || die "清理失败：$d"
    log "clean" "已删除 $d"
  else
    log "clean" "无需清理"
  fi
}

main() {
  parse_args "$@"
  load_cfg
  if ((${#ARGS[@]})); then set -- "${ARGS[@]}"; else set --; fi
  case "${1:-}" in
    build)   shift; do_build   "${1:-release}" ;;
    package) shift; do_package "${1:-release}" ;;
    release) shift; cfgv="${1:-release}"; do_build "$cfgv"; do_package "$cfgv" ;;
    clean)   do_clean ;;
    ""|-h|--help|help) usage; exit 0 ;;
    *) die "未知命令：$1（./run.sh help）" ;;
  esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
