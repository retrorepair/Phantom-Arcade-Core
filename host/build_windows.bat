@echo off
REM ===========================================================================
REM 1-Click Build Script for Phantom Arcade Windows Setup Application
REM Requires Microsoft Visual C++ (MSVC) or Visual Studio Build Tools
REM ===========================================================================

echo Building Phantom Arcade Manager (C++ Win32)...

cl.exe /std:c++17 /O2 /DUNICODE /D_UNICODE /EHsc PhantomArcadeManager.cpp /link /SUBSYSTEM:WINDOWS ws2_32.lib comctl32.lib shell32.lib user32.lib gdi32.lib /OUT:PhantomArcadeManager.exe

if %ERRORLEVEL% EQU 0 (
    echo.
    echo =======================================================
    echo  SUCCESS: PhantomArcadeManager.exe built successfully!
    echo =======================================================
    echo Run PhantomArcadeManager.exe to configure your arcade bridge.
) else (
    echo.
    echo [ERROR] Build failed. Verify you are running from "Developer Command Prompt for VS".
)
