. $env:QUBES_TOOLS\qubes-rpc-services\VMExec-Decode.ps1

try {
    $parts = ($args[0]).split("+")
    $decoded = @($parts.foreach({VMExec-Decode $_}))
    $cmd = $decoded -join " "
    & cmd.exe /c $cmd
} catch [DecodeError] {
    Write-Error $_.Exception.Message
}
