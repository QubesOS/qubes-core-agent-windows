class DecodeError : Exception {
    DecodeError([string] $msg): base($msg) {}
}

function VMExec-Decode {
    param (
        [string]$part
    )

    if ($part -notmatch '^[a-zA-Z0-9._-]*$') {
        throw [DecodeError]'Illegal characters found'
    }

    $ESCAPE_RE = [regex]::new('(--)|-([A-F0-9]{2})')

    # Check if no '-' remains outside of legal escape sequences.
    if ($part -contains ($ESCAPE_RE -replace '--|-([A-F0-9]{2})')) {
        throw [DecodeError]"'-' can be used only in '-HH' or '--' escape sequences"
    }

    $decodedPart = $ESCAPE_RE.Replace($part, {
        param($m)
        if ($m.Groups[1].Success) {
            return '-'
        } else {
            $num = [Convert]::ToInt32($m.Groups[2].Value, 16)
            return [System.Text.Encoding]::ASCII.GetString($num)
        }
    })

    return $decodedPart
}
