@ECHO OFF

IF NOT EXIST SIGN_CONFIG.BAT GOTO DONT_SIGN

IF %_BUILDARCH%==x86 (SET ARCHDIR=i386) ELSE (SET ARCHDIR=amd64)

for /R %%f in (*.exe) do %SIGNTOOL% sign /v %CERT_CROSS_CERT_FLAG% /f %CERT_FILENAME% %CERT_PASSWORD_FLAG% /t http://timestamp.verisign.com/scripts/timestamp.dll %%f

:DONT_SIGN
