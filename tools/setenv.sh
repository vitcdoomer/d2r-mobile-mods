#!/bin/bash
# d2r-mobile NO_XCODE 做包环境（source 后使用）
# 用法: source setenv.sh
export PATH="/e/10-installed/bin-shim:/e/10-installed/msys2/mingw64/bin:/e/10-installed/Python314/Scripts:/e/10-installed/Python314:/e/10-installed/dotnet10:$PATH"
export DM_ROOT="E:/02-gitee/07-IOS/diabilo/work/dmroot"
export DOTNET_ROOT="E:/10-installed/dotnet10"
export PYTHONUTF8="1"
echo "[setenv] 环境就绪："
echo "  gcc:          $(which gcc 2>/dev/null || echo 未找到)"
echo "  dotnet:       $(which dotnet 2>/dev/null || echo 未找到)"
echo "  spirv-cross:  $(which spirv-cross 2>/dev/null || echo 未找到)"
echo "  python:       $(which python 2>/dev/null || echo 未找到)"
echo "  DM_ROOT:      $DM_ROOT"
