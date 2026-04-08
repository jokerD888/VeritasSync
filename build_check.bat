@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul 2>&1
cd /d D:\project\VeritasSync\build\debug
cmake --build . --target veritas_sync 2>&1
