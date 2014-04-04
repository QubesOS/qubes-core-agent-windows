# Instantiate the wscript.shell COM object
$WshShell = new-object -comobject "WScript.Shell" 

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

    Write-Host "$($linkBaseName):Name=$appmenuLocation$($pathObj.BaseName)"
    Write-Host "$($linkBaseName):Exec=$($targetPath.Replace('\','\\'))"
    Write-Host "$($linkBaseName):Comment=$($linkObj.Description)"
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
