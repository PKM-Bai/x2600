[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$clonerRoot = Join-Path $workspaceRoot 'tools\cloner-windows\cloner-2.5.54-windows_alpha\cloner-2.5.54-windows_alpha'
$platformPath = Join-Path $clonerRoot 'configs\platforms.cfg'
if (-not (Test-Path -LiteralPath $platformPath -PathType Leaf)) {
    throw 'Install the complete Windows Cloner 2.5.54 package first with scripts\install-cloner.ps1.'
}

$boardName = 'x2600m_halley7m_libbare.cfg'
$boardPath = Join-Path $clonerRoot "configs\x2600\$boardName"
$templatePath = Join-Path $workspaceRoot 'configs\cloner\halley7m-libbare.cfg.in'
$template = [System.IO.File]::ReadAllText($templatePath)
$rendered = $template.Replace('@WORKSPACE@', $workspaceRoot.Replace('\', '/'))
[System.IO.File]::WriteAllText($boardPath, $rendered, [System.Text.UTF8Encoding]::new($false))

$section = ''
$platformLines = foreach ($line in [System.IO.File]::ReadAllLines($platformPath)) {
    if ($line -match '^\[(.+)\]$') {
        $section = $Matches[1]
    }
    if ($section -eq 'current' -and $line -match '^board=') {
        "board=$boardName"
    }
    elseif ($section -eq 'current' -and $line -match '^platform=') {
        'platform="13,x2600"'
    }
    else {
        $line
    }
}
[System.IO.File]::WriteAllLines($platformPath, [string[]]$platformLines, [System.Text.UTF8Encoding]::new($false))

[PSCustomObject]@{
    BoardConfig = $boardPath
    Platform = 'x2600'
    EraseMode = 'Part Erase'
}
