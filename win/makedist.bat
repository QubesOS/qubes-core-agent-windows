@echo off

SETLOCAL ENABLEEXTENSIONS
IF NOT EXIST set_ddk_path.bat ECHO >set_ddk_path.bat SET DDK_PATH=C:\WinDDK\7600.16385.1
rem IF NOT EXIST set_ddk_path_2k.bat ECHO >set_ddk_path_2k.bat SET DDK_PATH_2K=C:\WinDDK\6001.18002

FOR /F %%V IN (..\version_win) DO SET VERSION=%%V

SET /A NEW_BUILD_NUMBER=%BUILD_NUMBER%+1
ECHO >build_number.bat SET BUILD_NUMBER=%NEW_BUILD_NUMBER%

ECHO BUILDING %VERSION%

CALL set_ddk_path.bat
rem CALL set_ddk_path_2K.bat

SET CORE_DIR=%CD%
SET VCHAN_DIR=%CD%\..\vchan

mkdir symbols\%VERSION%

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk WIN7 && CD /D "%VCHAN_DIR%" && build -cZg"
cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk WIN7 && CD /D "%CORE_DIR%" && build -cZg && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk x64 WIN7 && CD /D "%VCHAN_DIR%" && build -cZg"
cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk x64 WIN7 && CD /D "%CORE_DIR%" && build -cZg && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre WIN7 && CD /D "%VCHAN_DIR%" && build -cZg"
cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre WIN7 && CD /D "%CORE_DIR%" && build -cZg && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre x64 WIN7 && CD /D "%VCHAN_DIR%" && build -cZg"
cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre x64 WIN7 && CD /D "%CORE_DIR%" && build -cZg && call wix.bat"

