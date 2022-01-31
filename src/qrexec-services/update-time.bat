@echo off

pushd "%QUBES_TOOLS%"\bin

qrexec-client-vm.exe @default^|qubes.GetDate^|powershell.exe -executionpolicy bypass -noninteractive -inputformat none -file "%QUBES_TOOLS%\qubes-rpc-services\set-time.ps1"

popd
