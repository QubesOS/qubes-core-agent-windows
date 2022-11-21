@echo off
if defined DEBUG @echo on

setlocal enabledelayedexpansion

set LOCALPORT=
set DOMAIN=
set FINALPORT=

set INPUT=%1
set REPLACED=!INPUT::=temp:!

for /f "tokens=1 delims=:" %%i in ("!REPLACED!") do set LOCALPORT=%%i
for /f "tokens=2 delims=:" %%i in ("!REPLACED!") do set DOMAIN=%%i
for /f "tokens=3 delims=:" %%i in ("!REPLACED!") do set FINALPORT=%%i

if not defined LOCALPORT goto help
if not defined DOMAIN goto help
if not defined FINALPORT goto help

if "%LOCALPORT:~0,-4%" == "" (set FINALLOCALPORT=%FINALPORT%) else (set FINALLOCALPORT=%LOCALPORT:~0,-4%)
if "%DOMAIN:~0,-4%" == "" (set FINALDOMAIN=@default) else (set FINALDOMAIN=%DOMAIN:~0,-4%)

if 1%FINALLOCALPORT% NEQ +1%FINALLOCALPORT% goto port
if 1%FINALPORT% NEQ +1%FINALPORT% goto port

if %FINALLOCALPORT% LSS 1 goto port
if %FINALPORT% LSS 1 goto port

if %FINALLOCALPORT% GTR 65535 goto port
if %FINALPORT% GTR 65535 goto port

echo Binding TCP '%FINALDOMAIN%:%FINALPORT%' to 'localhost:%FINALLOCALPORT%'...

pushd %~dp0..\bin\
START qrexec-client-vm.exe %FINALDOMAIN%^|qubes.ConnectTCP+%FINALPORT%^|qubes.tcp-listen+%FINALLOCALPORT%
popd

exit /b

:help
echo "Usage: qvm-connect-tcp [localport]:[vmname]:[port]"
echo "Bind localport to another VM port using the qubes.ConnectTCP RPC service."
exit /b

:port
echo "Invalid port provided"
exit /b
