function LogStart {
    $logDir = Get-ItemPropertyValue "hklm:\Software\Invisible Things Lab\Qubes Tools" LogDir
    # get the originating script path
    $basePath = (Get-PSCallStack | Where-Object { $_.ScriptName } | Select-Object -Last 1).ScriptName
    $baseName = Split-Path -Leaf -Path $basePath
    $logname = "$baseName-$(Get-Date -Format "yyyyMMdd-HHmmss")-$PID.log"
    $global:qwtLogPath = "$logDir\$logName"
    $global:qwtLogLevel = Get-ItemPropertyValue "hklm:\Software\Invisible Things Lab\Qubes Tools" LogLevel
}

function Log {
    param (
        [ValidateRange(1,5)][int]$level,
        [string]$msg
    )

    if ($level -le $qwtLogLevel) {
        $ts = Get-Date -Format "yyyyMMdd.HHmmss.fff"
        Add-Content $qwtLogPath -value "[$ts-$("EWIDV"[$level-1])] $msg"
    }
}

function LogError {
    param([string]$msg)
    Log 1 $msg
}

function LogWarning {
    param([string]$msg)
    Log 2 $msg
}

function LogInfo {
    param([string]$msg)
    Log 3 $msg
}

function LogDebug {
    param([string]$msg)
    Log 4 $msg
}

function LogVerbose {
    param([string]$msg)
    Log 5 $msg
}
