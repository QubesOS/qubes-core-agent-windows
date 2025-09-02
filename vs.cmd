for %%p in ("%cd%\..") do set QUBES_REPO=%%~fp\artifacts
set QB_LOCAL=1
set QB_SCRIPTS=%QUBES_BUILDER%\qubesbuilder\plugins\build_windows\scripts

:: Find EWDK, needed to build native executables

if "%EnterpriseWDK%" == "" (
  if "%EWDK_PATH%" == "" (
    for /f "skip=1" %%i in ('wmic logicaldisk get caption') do (
      if exist "%%i\LaunchBuildEnv.cmd" (
        set EWDK_PATH=%%i
        goto end_search
      )
    )
  )

  :end_search
  if "%EWDK_PATH%" == "" (
    echo [!] EWDK not found, mount the ISO or set the EWDK_PATH env variable
    exit /b 1
  )
)

:: Get the SDK version
for /f %%i in ('dir /d /b "%EWDK_PATH%\Program Files\Windows Kits\10\Include\10*"') do (
  set SDK_VER=%%i
  goto end
)

:end

:: Set paths for native executables
set EWDK_INCLUDES=%EWDK_PATH%\Program Files\Windows Kits\10\Include\%SDK_VER%\km
set EWDK_LIBS=%EWDK_PATH%\Program Files\Windows Kits\10\Lib\%SDK_VER%\um\x64

start vs2022\core-agent-windows.sln
