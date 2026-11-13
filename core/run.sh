#!/usr/bin/env bash
# porism 构建 / 打包脚本
# 支持: MinGW-w64 (MSYS2 的 MINGW64 / UCRT64 / CLANG64 终端) 与 Linux
set -euo pipefail

VERSION="${PORISM_VERSION:-1.0.0}"       # 同时作为 Linux .so 的完整版本
SOVER="${PORISM_SOVER:-1}"               # SONAME 主版本
CC="${CC:-gcc}"
AR="${AR:-ar}"
WERROR="${PORISM_WERROR:-0}"             # 1 = 追加 -Werror，建议 CI 里开启

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
SRC="$SCRIPT_DIR/porism.c"
DIST="$SCRIPT_DIR/dist"

log()  { printf '[%s] %s\n' "$1" "$2"; }
warn() { printf '[警告] %s\n' "$*" >&2; }
die()  { printf '[错误] %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

usage() {
  cat <<'EOF'
用法: ./run.sh <命令> [配置]

命令:
  build   [release|debug]   构建当前平台产物到 dist/<platform>-<arch>/
  package [release|debug]   把已构建产物整理成发布压缩包
  release [release|debug]   build + package（推荐）
  clean                     删除整个 dist/
  help                      显示本帮助

配置默认 release。release = -Os -s；debug = -O0 -g 且保留符号。

环境变量:
  PORISM_VERSION   版本号，默认 1.0.0
  PORISM_SOVER     SONAME 主版本，默认 1
  PORISM_WERROR    设为 1 追加 -Werror，默认 0
  CC / AR          覆盖编译器与归档器

打包所需的可选文件（放在脚本同目录，缺失只告警不中断）:
  include/ 或 porism.h               SDK 头文件
  LICENSE / LICENSE.txt / LICENSE.md
  README.md、CHANGELOG.md
EOF
}

# ---------- 平台识别与编译参数（全局，build / package 共用） ----------
detect_platform() {
  local u m
  u="$(uname -s)"
  m="$(uname -m)"
  case "$u" in
    MINGW*_NT-*)  PLATFORM=windows ;;
    Linux)        PLATFORM=linux ;;
    CYGWIN*|MSYS*|Windows_NT)
      die "检测到 $u：请在 MSYS2 的 MINGW64 / UCRT64 / CLANG64 终端里构建。MSYS/Cygwin 产物依赖 POSIX 仿真层，不适合发布。" ;;
    *) die "不支持的平台: $u（当前仅支持 Windows / Linux）" ;;
  esac
  case "$m" in
    x86_64|amd64)        ARCH=x86_64 ;;
    aarch64|arm64)       ARCH=aarch64 ;;
    i386|i486|i586|i686) ARCH=x86 ;;
    armv7*)              ARCH=armv7 ;;
    *) ARCH="$m"; warn "未识别架构 $m，包名沿用原值，请自行核对产物位数" ;;
  esac
  OUT="$DIST/${PLATFORM}-${ARCH}"
}

setup_flags() {
  CFG="$1"
  case "$CFG" in
    release|debug) ;;
    *) die "未知配置: $CFG（可选 release|debug）" ;;
  esac

  CFLAGS=(-std=c11 -Wall -Wextra)
  if [[ "$CFG" == release ]]; then
    CFLAGS+=(-Os)
    LINK_FLAGS=(-s)
    CFG_SUFFIX=""
  else
    CFLAGS+=(-O0 -g)
    LINK_FLAGS=()
    CFG_SUFFIX="-debug"
  fi
  if [[ "$WERROR" == "1" ]]; then
    CFLAGS+=(-Werror)
  fi

  case "$PLATFORM" in
    windows)
      EXE=porism.exe
      DLL=porism.dll
      STATIC=libporism.a
      OBJ=porism.o
      LINK_FLAGS+=(-static-libgcc)
      ;;
    linux)
      EXE=porism.linux
      DLL="libporism.so.$VERSION"
      STATIC=libporism_linux.a
      OBJ=porism_linux.o
      CFLAGS+=(-fPIC -pthread)
      LINK_FLAGS+=(-pthread)
      ;;
  esac

  # 链接参数只用于链接步骤；Step 3 的 gcc -c 不再喂这些标志
  LIBS=(-lm)
  if (( ${#LINK_FLAGS[@]} )); then
    LIBS+=("${LINK_FLAGS[@]}")
  fi
}

# ---------- 构建 ----------
verify_build() {
  step "构建自检"
  if command -v file >/dev/null 2>&1; then
    find "$OUT/bin" "$OUT/lib" -maxdepth 1 \( -type f -o -type l \) -print0 \
      | xargs -0 -r file || true
  fi
  if [[ "$PLATFORM" == linux ]]; then
    if command -v readelf >/dev/null 2>&1; then
      readelf -d "$OUT/lib/$DLL" | grep -i soname || true
    fi
    if command -v ldd >/dev/null 2>&1; then
      printf '可执行文件依赖:\n'
      ldd "$OUT/bin/$EXE" || true
    fi
  else
    if command -v objdump >/dev/null 2>&1; then
      printf '可执行文件导入的 DLL:\n'
      objdump -p "$OUT/bin/$EXE" | grep -i 'DLL Name' || true
    fi
  fi
}

print_sizes() {
  log "build" "编译完成"
  printf '  %-28s %12s\n' "文件" "体积"
  find "$OUT/bin" "$OUT/lib" -maxdepth 1 -type f -printf '%f %s\n' 2>/dev/null \
    | sort | while read -r n s; do
        printf '  %-28s %12s bytes\n' "$n" "$s"
      done
}

do_build() {
  local cfg="${1:-release}" obj
  detect_platform
  setup_flags "$cfg"

  [[ -f "$SRC" ]] || die "找不到源码 $SRC"
  command -v "$CC" >/dev/null 2>&1 || die "找不到编译器 $CC"
  command -v "$AR" >/dev/null 2>&1 || die "找不到归档器 $AR"

  log "build" "配置=$CFG  平台=$PLATFORM-$ARCH  版本=$VERSION"
  log "build" "输出目录=$OUT"

  # 只清本平台的输出目录，不动 dist/ 下其他平台与 staging
  rm -rf -- "$OUT"
  mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/.obj"
  obj="$OUT/.obj/$OBJ"

  step "1/3 可执行文件"
  "$CC" "${CFLAGS[@]}" "$SRC" -o "$OUT/bin/$EXE" "${LIBS[@]}"

  step "2/3 动态库"
  if [[ "$PLATFORM" == windows ]]; then
    # MS_DLL 按命名推断为「Windows 导出」开关，只在 Windows 定义
    "$CC" "${CFLAGS[@]}" -shared -DMS_LIB -DMS_DLL "$SRC" \
      -o "$OUT/bin/$DLL" "${LIBS[@]}" \
      -Wl,--out-implib,"$OUT/lib/libporism.dll.a"
  else
    "$CC" "${CFLAGS[@]}" -shared -DMS_LIB "$SRC" \
      -o "$OUT/lib/$DLL" "${LIBS[@]}" \
      "-Wl,-soname,libporism.so.$SOVER" -Wl,--no-undefined
    ln -sf "$DLL" "$OUT/lib/libporism.so.$SOVER"
    ln -sf "libporism.so.$SOVER" "$OUT/lib/libporism.so"
  fi

  step "3/3 静态库"
  "$CC" "${CFLAGS[@]}" -c -DMS_LIB "$SRC" -o "$obj"
  "$AR" rcs "$OUT/lib/$STATIC" "$obj"
  rm -rf -- "$OUT/.obj"          # .o 不留在构建目录，更不进发布包

  verify_build
  print_sizes
}

# ---------- 打包 ----------
do_package() {
  local cfg="${1:-release}" stage pkgname archive f
  detect_platform
  setup_flags "$cfg"

  [[ -d "$OUT/bin" && -d "$OUT/lib" ]] \
    || die "尚未构建 $OUT，请先执行 ./run.sh build $CFG"

  pkgname="porism-${VERSION}-${PLATFORM}-${ARCH}${CFG_SUFFIX}"
  stage="$DIST/stage/$pkgname"
  rm -rf -- "$stage"
  mkdir -p "$stage/bin" "$stage/lib"

  step "整理发布目录"
  cp -a "$OUT/lib/." "$stage/lib/"          # cp -a 保留 .so 软链
  if [[ "$PLATFORM" == windows ]]; then
    cp -a "$OUT/bin/$EXE" "$OUT/bin/$DLL" "$stage/bin/"
  else
    install -m 755 "$OUT/bin/$EXE" "$stage/bin/porism"     # porism.linux -> porism
    if [[ -f "$stage/lib/$STATIC" ]]; then
      mv "$stage/lib/$STATIC" "$stage/lib/libporism.a"     # 去掉 _linux 后缀
    fi
  fi

  if [[ -d "$SCRIPT_DIR/include" ]]; then
    cp -a "$SCRIPT_DIR/include" "$stage/include"
  elif [[ -f "$SCRIPT_DIR/porism.h" ]]; then
    mkdir -p "$stage/include"
    cp -a "$SCRIPT_DIR/porism.h" "$stage/include/"
  else
    warn "未找到 include/ 或 porism.h：SDK 用户无法链接，发布前请补头文件"
  fi

  for f in LICENSE LICENSE.txt LICENSE.md README README.md CHANGELOG CHANGELOG.md; do
    if [[ -f "$SCRIPT_DIR/$f" ]]; then
      cp -a "$SCRIPT_DIR/$f" "$stage/"
    fi
  done
  if [[ ! -f "$stage/LICENSE" && ! -f "$stage/LICENSE.txt" && ! -f "$stage/LICENSE.md" ]]; then
    warn "未找到 LICENSE*：发布前请补许可证文件"
  fi

  ( cd "$stage" && find . -type f ! -name SHA256SUMS -print0 \
      | sort -z | xargs -0 -r sha256sum > SHA256SUMS )

  archive="$DIST/porism-${VERSION}-${PLATFORM}-${ARCH}${CFG_SUFFIX}"
  step "生成压缩包"
  if [[ "$PLATFORM" == linux ]]; then
    archive="${archive}.tar.gz"
    tar -czf "$archive" -C "$DIST/stage" "$pkgname"
  else
    archive="${archive}.zip"
    if command -v zip >/dev/null 2>&1; then
      ( cd "$DIST/stage" && zip -qr "$archive" "$pkgname" )
    else
      warn "已整理好的目录保留在: $stage"
      die "找不到 zip，请安装（MSYS2: pacman -S zip）后重跑 ./run.sh package，或手动压缩该目录"
    fi
  fi

  log "package" "完成: $archive"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$archive"
  fi
  printf '包内顶层目录: %s/\n' "$pkgname"
  if [[ "$PLATFORM" == windows ]]; then
    printf '链接提示: lib/libporism.dll.a 是 DLL 导入库；强制静态链接请用 -l:libporism.a\n'
  else
    printf '链接提示: -lporism 默认走 .so；强制静态链接请用 -l:libporism.a\n'
  fi
}

do_clean() {
  if [[ -d "$DIST" ]]; then
    rm -rf -- "$DIST"
    log "clean" "已删除 $DIST"
  else
    log "clean" "无需清理：$DIST 不存在"
  fi
}

# ---------- 入口 ----------
case "${1:-}" in
  build)   shift; do_build   "${1:-release}" ;;
  package) shift; do_package "${1:-release}" ;;
  release) shift; cfg="${1:-release}"; do_build "$cfg"; do_package "$cfg" ;;
  clean)   shift; do_clean ;;
  ""|-h|--help|help) usage; exit 0 ;;
  *) die "未知命令: $1（执行 ./run.sh help 查看用法）" ;;
esac
