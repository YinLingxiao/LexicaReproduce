@echo off
setlocal
rem 从脚本所在目录执行，避免双击时工作目录不正确。
cd /d "%~dp0"
rem 优先使用 MSYS2 UCRT64 工具链。
if exist C:\msys64\ucrt64\bin\g++.exe set "PATH=C:\msys64\ucrt64\bin;%PATH%"
rem 先生成构建文件，成功后再编译。
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b 1
cmake --build build
