#!/usr/bin/env bash
# porism 构建 / 打包（MSYS2 MinGW / Linux / WSL）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

log()  { printf '[%s] %s\n' "$1" "$2"; }
step() { printf '[package] %s\n' "$*"; }
warn() { printf '[警告] %s\n' "$*" >&2; }
die()  { printf '[错误] %s\n' "$*" >&2; exit 1; }
cfg()  { local v="${!1-}"; if [[ -n "$v" ]]; then printf '%s' "$v"; else printf '%s' "$2"; fi; }

declare -a ARGS=()

# 允许 KEY=VALUE / -KEY=VALUE / --KEY=VALUE 出现在任意位置
parse_args() {
  local -a pos=()
  local a k
  for a in "$@"; do
    if [[ "$a" =~ ^-{0,2}([A-Z][A-Z0-9_]*)=(.*)$ ]]; then
      k="${BASH_REMATCH[1]}"
      case "$k" in
        PORISM_VERSION|PORISM_SOVER|PORISM_WERROR|PORISM_SRC|PORISM_DOC_DIR|PORISM_DIST|\
        PORISM_DLLTOOL|PORISM_ARCHIVE|PORISM_TOPDIR|PORISM_7Z|PORISM_SUMS|CC|AR|TMPDIR|MINGW_PACKAGE_PREFIX)
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
  VERSION="$(cfg PORISM_VERSION 1.0.0)"
  SOVER="$(cfg PORISM_SOVER 1)"
  CC="$(cfg CC gcc)"
  AR="$(cfg AR ar)"
  WERROR="$(cfg PORISM_WERROR 0)"
  DOC_DIR="$(cfg PORISM_DOC_DIR "$SCRIPT_DIR/..")"
  TOPDIR="$(cfg PORISM_TOPDIR 0)"
  SUMS="$(cfg PORISM_SUMS outside)"
  TMP_PARENT="$(cfg TMPDIR /tmp)"
}

usage() {
  cat <<'EOF'
用法: ./run.sh <命令> [配置] [-KEY=VALUE ...]

命令:
  build   [release|debug]   构建到 <输出目录>/<platform>-<arch>/
  package [release|debug]   整理成发布包
  release [release|debug]   build + package
  tools                     查看当前配置与打包器
  clean                     删除输出目录
  help                      本帮助

参数形式（KEY=VALUE 前可带 - 或 --，位置不限）:
  ./run.sh release -PORISM_7Z='I:\7-Zip\7z.exe'
  ./run.sh release -PORISM_7Z=I:/7-Zip/7z.exe -PORISM_ARCHIVE=7z
  ./run.sh -PORISM_TOPDIR=1 release

  路径含反斜杠时请加引号，否则 bash 会吞掉 \；脚本会自动兜底查找。
  也可用传统写法：PORISM_ARCHIVE=7z ./run.sh release

可用变量:
  PORISM_VERSION  版本号，默认 1.0.0（同时写进二进制，--version 与包名一致）
  PORISM_SOVER    SONAME 主版本，默认 1
  PORISM_WERROR   1 = 追加 -Werror
  PORISM_SRC      porism.c 路径
  PORISM_DOC_DIR  资源备用目录，默认脚本上级目录
  PORISM_DIST     输出目录，默认脚本同目录 dist/
  PORISM_DLLTOOL  生成 .lib 的工具（按 llvm-dlltool 参数调用）
  PORISM_ARCHIVE  zip / 7z / tar.gz（默认 Windows=zip，Linux=tar.gz）
  PORISM_TOPDIR   1 = 包内保留一层顶层目录，默认 0 = 平铺
  PORISM_7Z       7z 可执行文件（Windows 路径或 MSYS2 路径均可）
  PORISM_SUMS     outside（默认）= 额外生成 <包名>.sha256 校验压缩包本身
                  both = 同时在包内保留 SHA256SUMS 文件清单
  CC / AR / TMPDIR

资源查找：脚本同目录 → PORISM_DOC_DIR；缺头文件/LICENSE 只告警。
Windows 包 lib/ 含 porism.lib、porism.def、libporism.dll.a、libporism.a。
打包器：装 Windows 版 7-Zip 后用 -PORISM_7Z 指定；或 pacman -S p7zip / zip。
编译工具：MSYS2 MinGW 终端 pacman -S --needed $MINGW_PACKAGE_PREFIX-gcc $MINGW_PACKAGE_PREFIX-binutils $MINGW_PACKAGE_PREFIX-tools
EOF
}

# ---------- 路径与打包器 ----------
# I:\7-Zip\7z.exe -> /i/7-Zip/7z.exe（兼容 WSL 的 /mnt/i）
# 若反斜杠被 shell 吞掉（I:7-Zip7z.exe），在盘根下兜底找 7z*.exe
norm_7z_path() {
  local v="$1" p d rest root found
  p="${v//\\//}"
  if [[ "$p" =~ ^([A-Za-z]):/(.*)$ ]]; then
    d="${BASH_REMATCH[1],,}"
    rest="${BASH_REMATCH[2]}"
    for root in "/$d" "/mnt/$d"; do
      if [[ -f "$root/$rest" ]]; then printf '%s\n' "$root/$rest"; return 0; fi
    done
    printf '%s\n' "/$d/$rest"
    return 0
  fi
  if [[ "$v" =~ ^([A-Za-z]):(.+)$ && "$v" != */* && "$v" != *\\* ]]; then
    d="${BASH_REMATCH[1],,}"
    for root in "/$d" "/mnt/$d"; do
      [[ -d "$root" ]] || continue
      found="$(find "$root" -maxdepth 4 -type f -iname '7z*.exe' -print -quit 2>/dev/null || true)"
      if [[ -n "$found" ]]; then
        warn "反斜杠被 shell 吞掉，已定位 $found（建议给路径加引号）"
        printf '%s\n' "$found"
        return 0
      fi
    done
  fi
  printf '%s\n' "$p"
}

find_7z() {
  local p c
  p="$(cfg PORISM_7Z '')"
  if [[ -n "$p" ]]; then
    c="$(norm_7z_path "$p")"
    [[ -f "$c" ]] || return 1
    printf '%s\n' "$c"
    return 0
  fi
  for c in 7z 7zz 7za "/c/Program Files/7-Zip/7z.exe" "/c/Program Files (x86)/7-Zip/7z.exe"; do
    if [[ "$c" == */* ]]; then
      if [[ -x "$c" ]]; then printf '%s\n' "$c"; return 0; fi
    elif command -v "$c" >/dev/null 2>&1; then
      printf '%s\n' "$c"; return 0
    fi
  done
  return 1
}

archive_fmt() {
  local f
  f="$(cfg PORISM_ARCHIVE '')"
  if [[ -z "$f" ]]; then
    if [[ "$PLATFORM" == windows ]]; then f=zip; else f=tar.gz; fi
  fi
  case "$f" in zip|7z|tar.gz) ;; *) die "未知 PORISM_ARCHIVE：$f（zip / 7z / tar.gz）" ;; esac
  printf '%s' "$f"
}

find_aux() {
  local n="$1" d
  for d in "$SCRIPT_DIR" "$DOC_DIR"; do
    if [[ -e "$d/$n" ]]; then printf '%s\n' "$d/$n"; return 0; fi
  done
  return 1
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

  if [[ "$PLATFORM" == linux && ( "$SCRIPT_DIR" == /mnt/?/* || "$DIST" == /mnt/?/* ) ]]; then
    warn "当前在 WSL/Windows 挂载盘：产物是 Linux ELF；要 .exe/.dll 请用 MSYS2 的 UCRT64/MINGW64 终端"
  fi
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

# ---------- 构建 ----------
# GNU ld 的 .dll.a 不是 MSVC 原生导入库；优先从同次链接产出的 .def 重建
build_msvc_implib() {
  local def="$OUT/lib/porism.def" out="$OUT/lib/porism.lib" m lm tool wdef wout
  case "$ARCH" in
    x86_64)  m=X64;   lm="i386:x86-64" ;;
    x86)     m=X86;   lm="i386" ;;
    aarch64) m=ARM64; lm="aarch64" ;;
    *)       m=ARM;   lm="arm" ;;
  esac
  if [[ ! -s "$def" ]] && command -v gendef >/dev/null 2>&1; then
    ( cd "$OUT/lib" && gendef "$OUT/bin/$DLL" >/dev/null 2>&1 ) || true
  fi
  [[ -s "$def" ]] || return 1

  tool="$(cfg PORISM_DLLTOOL '')"
  if [[ -z "$tool" ]] && command -v llvm-dlltool >/dev/null 2>&1; then tool=llvm-dlltool; fi
  if [[ -n "$tool" ]] && "$tool" -m "$lm" -d "$def" -D "$DLL" -l "$out" >/dev/null 2>&1; then return 0; fi

  if command -v lib.exe >/dev/null 2>&1; then
    wdef="$def"; wout="$out"
    if command -v cygpath >/dev/null 2>&1; then wdef="$(cygpath -w "$def")"; wout="$(cygpath -w "$out")"; fi
    if MSYS2_ARG_CONV_EXCL='*' lib.exe /NOLOGO "/DEF:$wdef" "/MACHINE:$m" "/OUT:$wout" >/dev/null 2>&1; then return 0; fi
  fi
  if command -v dlltool >/dev/null 2>&1 && dlltool -d "$def" -D "$DLL" -l "$out" >/dev/null 2>&1; then return 0; fi
  return 1
}

do_build() {
  local cfg="${1:-release}" src obj
  detect_platform
  setup_flags "$cfg"

  src="$(cfg PORISM_SRC '')"
  [[ -n "$src" ]] || src="$(find_aux porism.c || true)"
  [[ -n "$src" && -f "$src" ]] || die "找不到 porism.c（可用 -PORISM_SRC= 指定）"
  command -v "$CC" >/dev/null 2>&1 || die "找不到编译器 $CC"
  command -v "$AR" >/dev/null 2>&1 || die "找不到归档器 $AR"

  log "build" "$PLATFORM-$ARCH $CFG $VERSION -> $OUT"
  rm -rf -- "$OUT"
  mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/.obj"
  obj="$OUT/.obj/$OBJ"

  "$CC" "${CFLAGS[@]}" "$src" -o "$OUT/bin/$EXE" "${LIBS[@]}"

  if [[ "$PLATFORM" == windows ]]; then
    # PORISM_LIB 去掉 main；PORISM_DLL 让导出表只有 PORISM_API 标记的函数。
    # 旧版写的 -DMS_LIB/-DMS_DLL 与源码宏名不符，导致静态库和 DLL 都带 main。
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
  rm -rf -- "$OUT/.obj"
}

# ---------- 打包 ----------
do_package() {
  local cfg="${1:-release}" stage stage_parent pkgname archive fmt f hr src dir
  local packer packer7z tflag archive_arg p7 sumfile hash
  detect_platform
  setup_flags "$cfg"

  [[ -d "$OUT/bin" && -d "$OUT/lib" ]] || die "尚未构建：$OUT"
  case "$SUMS" in outside|both) ;; *) die "未知 PORISM_SUMS：$SUMS（outside|both）" ;; esac

  fmt="$(archive_fmt)"
  p7="$(cfg PORISM_7Z '')"
  # tar.gz 不需要 7z，绝不探测：否则 Windows 盘路径会把 Linux 打包直接拦下。
  # zip/7z 才探测；显式指定了 7z 路径却找不到时，仅在必须用 7z 的格式下报错。
  packer7z=""
  if [[ "$fmt" != tar.gz ]]; then
    packer7z="$(find_7z || true)"
    if [[ -n "$p7" && -z "$packer7z" && "$fmt" == 7z ]]; then
      die "PORISM_7Z 指向的文件不存在：$p7"
    fi
  fi

  pkgname="porism-${VERSION}-${PLATFORM}-${ARCH}${CFG_SUFFIX}"
  stage_parent="$(mktemp -d "$TMP_PARENT/porism-stage.XXXXXX")" \
    || die "无法创建暂存目录：$TMP_PARENT"
  stage="$stage_parent/$pkgname"
  mkdir -p "$stage/bin" "$stage/lib"

  step "1/4 整理产物与文档 -> 暂存目录"
  cp -a "$OUT/lib/." "$stage/lib/"
  if [[ "$PLATFORM" == windows ]]; then
    cp -a "$OUT/bin/$EXE" "$OUT/bin/$DLL" "$stage/bin/"
  else
    install -m 755 "$OUT/bin/$EXE" "$stage/bin/porism"
  fi

  local -a dirs=("$SCRIPT_DIR" "$DOC_DIR")
  hr=""
  for dir in "${dirs[@]}"; do
    if [[ -d "$dir/include" ]]; then hr="$dir/include"; break; fi
    if [[ -f "$dir/porism.h" ]]; then hr="$dir/porism.h"; break; fi
  done
  if [[ -n "$hr" ]]; then
    mkdir -p "$stage/include"
    if [[ -d "$hr" ]]; then cp -a "$hr/." "$stage/include/"; else cp -a "$hr" "$stage/include/"; fi
  else
    warn "未找到 include/ 或 porism.h：SDK 用户无法链接"
  fi
  for f in LICENSE LICENSE.txt LICENSE.md README README.md CHANGELOG CHANGELOG.md; do
    src=""
    for dir in "${dirs[@]}"; do
      if [[ -f "$dir/$f" ]]; then src="$dir/$f"; break; fi
    done
    if [[ -n "$src" ]]; then cp -a "$src" "$stage/"; fi
  done
  if [[ ! -f "$stage/LICENSE" && ! -f "$stage/LICENSE.txt" && ! -f "$stage/LICENSE.md" ]]; then
    warn "未找到 LICENSE*"
  fi

  if [[ "$PLATFORM" == windows ]]; then
    step "2/4 检查 Windows 库文件（lib/ 四件）"
    for f in porism.lib porism.def libporism.dll.a libporism.a; do
      [[ -f "$stage/lib/$f" ]] || warn "包内缺少 lib/$f"
    done
  else
    step "2/4 Linux 改名与 SONAME 软链"
    if [[ -f "$stage/lib/$STATIC" ]]; then mv "$stage/lib/$STATIC" "$stage/lib/libporism.a"; fi
    ln -sfn "$DLL" "$stage/lib/libporism.so.$SOVER" 2>/dev/null \
      || cp -a "$stage/lib/$DLL" "$stage/lib/libporism.so.$SOVER"
    ln -sfn "libporism.so.$SOVER" "$stage/lib/libporism.so" 2>/dev/null \
      || cp -a "$stage/lib/$DLL" "$stage/lib/libporism.so"
  fi
  if [[ "$SUMS" == both ]]; then
    ( cd "$stage" && find . -type f ! -name SHA256SUMS -print0 \
        | sort -z | xargs -0 -r sha256sum > SHA256SUMS )
  fi

  step "3/4 压缩（$fmt / $([[ "$TOPDIR" == 1 ]] && echo 顶层目录 || echo 平铺)）"
  archive="$DIST/${pkgname}"
  case "$fmt" in
    tar.gz)
      archive="${archive}.tar.gz"
      if [[ "$TOPDIR" == 1 ]]; then
        tar -czf "$archive" -C "$stage_parent" "$pkgname"
      else
        ( cd "$stage" && find . -mindepth 1 -maxdepth 1 -print0 | sort -z \
            | tar --null -czf "$archive" -T - )
      fi
      ;;
    zip|7z)
      if [[ "$fmt" == 7z ]]; then
        archive="${archive}.7z"
        packer="$packer7z"
        tflag=-t7z
      else
        archive="${archive}.zip"
        tflag=-tzip
        if [[ "$PLATFORM" == windows ]]; then
          packer="$packer7z"
          if [[ -z "$packer" ]]; then packer="$(command -v zip || true)"; fi
        else
          packer="$(command -v zip || true)"
          if [[ -z "$packer" ]]; then packer="$packer7z"; fi
        fi
      fi
      [[ -n "$packer" ]] || die "缺打包器：指定 -PORISM_7Z=I:/7-Zip/7z.exe，或 pacman -S p7zip / zip"
      if [[ "$(basename "$packer")" == zip || "$(basename "$packer")" == zip.exe ]]; then
        if [[ "$TOPDIR" == 1 ]]; then
          ( cd "$stage_parent" && zip -qry "$archive" "$pkgname" )
        else
          ( cd "$stage" && zip -qry "$archive" . )
        fi
      else
        archive_arg="$archive"
        if [[ "$packer" == *.exe ]] && command -v cygpath >/dev/null 2>&1; then
          archive_arg="$(cygpath -w "$archive")"
        fi
        if [[ "$TOPDIR" == 1 ]]; then
          ( cd "$stage_parent" && "$packer" a "$tflag" -mx=9 -y -- "$archive_arg" "$pkgname" >/dev/null )
        else
          ( cd "$stage" && "$packer" a "$tflag" -mx=9 -y -- "$archive_arg" . >/dev/null )
        fi
        if [[ "$PLATFORM" == linux ]]; then
          warn "7z 打包 Linux 包：解压后可能需要 chmod +x bin/porism"
        fi
      fi
      ;;
  esac

  # 校验压缩包本身：用户下载后与 .sha256 放同目录，sha256sum -c 即可
  sumfile="${archive}.sha256"
  ( cd "$DIST" && sha256sum "${archive##*/}" > "${archive##*/}.sha256" )
  hash="$(cut -d' ' -f1 < "$sumfile")"
  step "4/4 校验和 ${archive##*/}.sha256  $hash"
  rm -rf -- "$stage_parent"
}

# ---------- 其他命令 ----------
do_tools() {
  local z fmt
  detect_platform
  fmt="$(archive_fmt)"
  z="$(find_7z || true)"
  log "tools" "$PLATFORM-$ARCH 归档=$fmt 布局=$([[ "$TOPDIR" == 1 ]] && echo 顶层目录 || echo 平铺)"
  log "tools" "7z=${z:-未找到} zip=$(command -v zip || echo 未找到) 输出=$DIST"
}

do_clean() {
  local d
  d="$(cfg PORISM_DIST "$SCRIPT_DIR/dist")"
  # 防止误传盘根 / 家目录 / 脚本根目录导致不可逆删除
  case "$d" in
    /|[A-Za-z]:/|/c|/c/|/mnt/?|/mnt/?/|"$HOME"|"$SCRIPT_DIR")
      die "拒绝清理危险目录：$d" ;;
  esac
  if [[ -d "$d" ]]; then
    rm -rf -- "$d"
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
    tools)   do_tools ;;
    clean)   do_clean ;;
    ""|-h|--help|help) usage; exit 0 ;;
    *) die "未知命令：$1（./run.sh help）" ;;
  esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
