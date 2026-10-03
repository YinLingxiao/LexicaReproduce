@echo off
setlocal
cd /d "%~dp0"
if exist C:\msys64\ucrt64\bin\g++.exe set "PATH=C:\msys64\ucrt64\bin;%PATH%"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b 1
cmake --build build
