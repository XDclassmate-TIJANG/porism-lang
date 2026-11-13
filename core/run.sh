#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
DIST="$SCRIPT_DIR/dist"

usage() {
  cat <<'EOF'
用法: ./run.sh <build|clean> [附加参数]
EOF
  exit 1
}

do_build() {
  local cfg="${1:-无}"
  echo "[build] 配置: $cfg"

  mkdir -p dist
  cd dist

  echo "[build] 配置平台"
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        PLATFORM=windows
        CFLAGS=(-std=c11 -Os -Wall -Wextra)
        LINK_FLAGS=(-s -static-libgcc)
        EXE=porism.exe
        DLL=porism.dll
        OBJ=porism.o
        STATIC=libporism.a
        ;;
    Linux)
        PLATFORM=linux
        CFLAGS=(-std=c11 -Os -Wall -Wextra -fPIC -pthread)
        LINK_FLAGS=(-s -pthread)
        EXE=porism.linux
        DLL=libporism.so
        OBJ=porism.o
        STATIC=libporism.a
        ;;
    *)
        echo "不支持的平台：$(uname -s)（当前仅支持 Windows / Linux）" >&2
        exit 1
        ;;
  esac
  echo "       平台：$PLATFORM"
  echo "       产物：$EXE / $DLL / $STATIC"
  
  LIBS=(-lm "${LINK_FLAGS[@]}")

  echo "[build] Step 1/3 可执行文件"
  gcc "${CFLAGS[@]}" ../porism.c -o "$EXE" "${LIBS[@]}"

  echo "[build] Step 2/3 动态库"
  gcc "${CFLAGS[@]}" -shared -DMS_LIB -DMS_DLL ../porism.c -o "$DLL" "${LIBS[@]}"
  
  echo "[build] Step 3/3 静态库"
  gcc "${CFLAGS[@]}" -c -DMS_LIB ../porism.c -o "$OBJ"
  ar rcs "$STATIC" "$OBJ"
  
  echo "[build] 编译完成"
  printf '  文件名                  体积\n'
  for f in "$EXE" "$DLL" "$STATIC"; do
      printf '  %-20s %8s bytes\n' "$f" "$(wc -c < "$f" | tr -d '[:space:]')"
  done
}

do_clean() {
  if [ -d "$DIST" ]; then
    rm -rf -- "$DIST"
    echo "[clean] 已删除 $DIST"
  else
    echo "[clean] 无需清理：$DIST 不存在"
  fi
}

case "${1:-}" in
  build) shift; do_build "$@" ;;
  clean) shift; do_clean "$@" ;;
  ""|-h|--help) usage ;;
  *) echo "未知命令: $1" >&2; usage ;;
esac