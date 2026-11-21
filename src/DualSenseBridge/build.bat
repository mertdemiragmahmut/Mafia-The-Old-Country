@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

set SRC_DIR=%~dp0
set DIST_DIR=%~dp0..\..\dist
set TOOLS_DIR=%~dp0..\..\tools

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"

pushd "%SRC_DIR%"
cl.exe /nologo /O2 /EHsc /W3 /std:c++17 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /LD ^
    dualsense_hid.cpp wasapi_haptics.cpp pipe_server.cpp dllmain.cpp ^
    /Fe:"%DIST_DIR%\xinput1_4.dll" ^
    /link /DEF:"xinput1_4.def" /IMPLIB:"%DIST_DIR%\xinput1_4.lib" ^
    hid.lib setupapi.lib ole32.lib avrt.lib user32.lib advapi32.lib
if errorlevel 1 (
    popd
    exit /b 1
)

cl.exe /nologo /O2 /EHsc /W3 /std:c++17 /MD /D_CRT_SECURE_NO_WARNINGS ^
    test_bridge.cpp ^
    /Fe:"%TOOLS_DIR%\test_bridge.exe" ^
    /link user32.lib
if errorlevel 1 (
    popd
    exit /b 1
)

del /q "%SRC_DIR%*.obj" >nul 2>&1
popd
echo Build succeeded: %DIST_DIR%\xinput1_4.dll and %TOOLS_DIR%\test_bridge.exe
exit /b 0
