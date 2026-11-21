[CmdletBinding()]
param(
    [string]$MelodiesPath
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '../firmware/music_box'
$local = Join-Path $env:LOCALAPPDATA 'Advent26'
$sketch = Join-Path $local 'music_box'
$config = Join-Path $local 'secrets.txt'
New-Item -ItemType Directory -Path $sketch -Force | Out-Null

if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    $previous = Join-Path $source 'secrets.h'
    if (-not (Test-Path -LiteralPath $previous -PathType Leaf)) {
        $previous = Join-Path $source 'secrets.example.h'
    }
    [IO.File]::Copy((Resolve-Path -LiteralPath $previous).Path, $config)
}

Get-ChildItem -LiteralPath $source -File |
    Where-Object {
        $_.Extension -in @('.ino', '.cpp', '.h') -and
        $_.Name -notin @('secrets.h', 'personal-melodies.h')
    } |
    Copy-Item -Destination $sketch -Force

if ($MelodiesPath) {
    Copy-Item -LiteralPath $MelodiesPath -Destination (Join-Path $sketch 'personal-melodies.h') -Force
}

$include = '#include "' + $config.Replace('\', '/') + '"' + [Environment]::NewLine
[IO.File]::WriteAllText((Join-Path $sketch 'secrets.h'), $include, [Text.UTF8Encoding]::new($false))
Write-Output "Edit credentials in $config"
Write-Output "Open and upload $(Join-Path $sketch 'music_box.ino')"
