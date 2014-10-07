# Registry location for path hash -> full path mapping for GetImageRGBA
$RegistryMapPath = 'HKLM:\Software\Invisible Things Lab\Qubes Tools'
$RegistryMapKey = 'AppMap'

New-Item -Path $RegistryMapPath -Name $RegistryMapKey -Force | Out-Null

# Instantiate the wscript.shell COM object
$WshShell = new-object -comobject "WScript.Shell"

$Sha1 = [System.Security.Cryptography.SHA1]::Create()
Function GetHash($string)
{
    $hash = [BitConverter]::ToString($Sha1.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($string))).Replace("-", "")
    return $hash.ToLowerInvariant()
}

# Extract data from one shortcut file
Function ProcessLink($pathObj, $basepath)
{
    $desktopFileName = ($pathObj.FullName).SubString($basepath.Length+1).Replace(' ', '_').Replace('\','-')
    $linkBaseName = $desktopFileName.Replace('.lnk', '.desktop') # fixme: check if it's at the end of the string
    if ($pathObj.DirectoryName -ne $basepath) {
        $appmenuLocation = $pathObj.DirectoryName.SubString($basepath.Length+1).Replace('\','-') + " "
    } else {
        $appmenuLocation = ""
    }

    $linkObj = $WshShell.CreateShortcut($pathObj.FullName)
    $targetPath = "cmd.exe /c `"$($pathObj.FullName)`""
    # We send .LNK file hash as icon name since the name can't contain some characters that can be in a file path
    # and the GetImageRGBA Qubes service needs to retrieve bitmap from this name alone.
    $targetHash = GetHash($pathObj.FullName)

    # Store the hash-LNK mapping in the registry for easy  retrieval by GetImageRGBA.
    New-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $targetHash -PropertyType String -Value $pathObj.FullName | Out-Null

    Write-Host "$($linkBaseName):Name=$appmenuLocation$($pathObj.BaseName)"
    Write-Host "$($linkBaseName):Exec=$($targetPath.Replace('\','\\'))"
    Write-Host "$($linkBaseName):Comment=$($linkObj.Description)"
    Write-Host "$($linkBaseName):Icon=$targetHash"
}

# todo: check if there are duplicated entries

# "All users" menu
$p = $WshShell.SpecialFolders.item("AllUsersPrograms")
$shortcuts = get-childitem -path $p -filter "*.lnk" -rec
$shortcuts | foreach-object {ProcessLink $_  $p }

# Current user menu
$p = $WshShell.SpecialFolders.item("StartMenu")
$shortcuts = get-childitem -path $p -filter "*.lnk" -rec
$shortcuts | foreach-object {ProcessLink $_  $p }
