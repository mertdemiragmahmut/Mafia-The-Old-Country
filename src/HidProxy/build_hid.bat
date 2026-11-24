@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

set SRC_DIR=%~dp0
set DIST_DIR=%~dp0..\..\dist

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

pushd "%SRC_DIR%"
ml64.exe /nologo /c /Fo"hid_forwards.obj" hid_forwards.asm
if errorlevel 1 (
    popd
    exit /b 1
)

cl.exe /nologo /O2 /EHsc /W3 /std:c++17 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /LD ^
    hid_proxy.cpp hid_forwards.obj ^
    /Fe:"%DIST_DIR%\hid.dll" ^
    /link /DEF:"hid.def" /IMPLIB:"%DIST_DIR%\hid_proxy.lib"
if errorlevel 1 (
    popd
    exit /b 1
)

del /q "%SRC_DIR%*.obj" >nul 2>&1
popd
echo Build succeeded: %DIST_DIR%\hid.dll
exit /b 0
