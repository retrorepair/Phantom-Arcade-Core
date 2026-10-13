@echo off
rem Build Fightcade FBNeo the way its project says: run_scripts.bat (perl-generated headers + the compiled-in game detectors),
rem then MSBuild Release|Win32. perl comes from MSYS2, nasm from MSYS2's mingw32.
rem The DirectX SDK (June 2010) is the Include/Lib extracted to a user folder (dx.targets).
rem
rem This session's environment sets NoDefaultCurrentDirectoryInExePath=1, which makes cmd skip the current directory when it
rem looks up a command. The project's own steps ("run_scripts.bat", "m68kmake.exe", both bare names) depend on that search, so
rem it is cleared for this build only.
set NoDefaultCurrentDirectoryInExePath=
set "PATH=C:\msys64\usr\bin;C:\msys64\mingw32\bin;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
set LOG=C:\Users\joelw\AppData\Local\Temp\fbneo-vs.log
set DX=C:\Users\joelw\AppData\Local\Temp\claude\C--Users-joelw-Documents-Phantom-Arcade-NLC-main\bab491e0-6fea-4cdc-a5ff-c4fcae45d7a4\scratchpad\dx.targets
set FBNEO_DETECTORS=C:\Users\joelw\Documents\groovy-forks\fightcade-detectors
echo == %DATE% %TIME% > %LOG%
cd /d C:\Users\joelw\Documents\groovy-forks\fightcade-fbneo\projectfiles\visualstudio-2015
call run_scripts.bat >> %LOG% 2>&1
cd /d C:\Users\joelw\Documents\groovy-forks\fightcade-fbneo
msbuild projectfiles\visualstudio-2015\fbneo_vs2015.vcxproj /m /nologo /v:minimal /p:Configuration=Release /p:Platform=Win32 /p:ForceImportAfterCppTargets=%DX% %* >> %LOG% 2>&1
echo EXIT=%ERRORLEVEL% >> %LOG%
