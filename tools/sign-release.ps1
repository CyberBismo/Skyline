# Skyline - Copyright (C) 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
# Signs a built Skyline.exe as the newest release:
#   powershell -ExecutionPolicy Bypass -File tools\sign-release.ps1 -Exe out\Skyline.exe `
#       -Notes "What's new, one line" -NotesNo "Hva er nytt, en linje"
#
# Writes release\latest.txt and release\Skyline-<version>.exe. Upload both to
# skyline.skybyn.com/update/ (the exe first, then latest.txt). Every running
# Skyline older than this version offers the update within ~6 hours.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Notes = '',
    [string]$NotesNo = '',
    [string]$KeyFile = "$env:USERPROFILE\skyline-signing.key",
    [string]$BaseUrl = 'https://skyline.skybyn.com/update',
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
# $PSScriptRoot is empty inside param() on Windows PowerShell 5.1
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot '..\release' }

$Exe = (Resolve-Path $Exe).Path
$version = (Get-Item $Exe).VersionInfo.ProductVersion
if ($version) { $version = $version.Trim() }
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Couldn't read a x.y.z version from $Exe (got '$version'). Set it in src\version.h." }
if (-not (Test-Path $KeyFile)) { throw "No signing key at $KeyFile. Get it from your co-developer, or run tools\new-signing-key.ps1 once." }

$key = [System.Security.Cryptography.CngKey]::Import([System.IO.File]::ReadAllBytes($KeyFile), [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)

# Refuse to sign with a key the exe doesn't trust (wrong key file, or update_key.h not built in).
$public = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
$headerText = Get-Content (Join-Path $PSScriptRoot '..\src\update_key.h') -Raw
$built = [byte[]]([regex]::Matches($headerText, '0x([0-9a-fA-F]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
if ($built.Length -ne $public.Length -or (Compare-Object $built $public -SyncWindow 0)) {
    throw "This key doesn't match src\update_key.h. Wrong key file?"
}

$bytes = [System.IO.File]::ReadAllBytes($Exe)
$sha = -join ([System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') })
$name = "Skyline-$version.exe"
$oneLine = { param($s) ($s -replace '[\r\n]+', ' ').Trim() }

$body = "skyline-update 1`n" +
        "version=$version`n" +
        "url=$BaseUrl/$name`n" +
        "size=$($bytes.Length)`n" +
        "sha256=$sha`n" +
        "notes=$(& $oneLine $Notes)`n" +
        "notes_no=$(& $oneLine $NotesNo)`n"
$utf8 = New-Object System.Text.UTF8Encoding($false)
$data = $utf8.GetBytes($body)
$sig = $ecdsa.SignData($data, [System.Security.Cryptography.HashAlgorithmName]::SHA256)   # IEEE P1363 r||s, 64 bytes
if ($sig.Length -ne 64 -or -not $ecdsa.VerifyData($data, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) {
    throw 'Signing failed.'
}
$manifest = $body + 'sig=' + (-join ($sig | ForEach-Object { $_.ToString('x2') })) + "`n"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $Exe (Join-Path $OutDir $name) -Force
[System.IO.File]::WriteAllBytes((Join-Path $OutDir 'latest.txt'), $utf8.GetBytes($manifest))

Write-Host "Signed Skyline $version"
Write-Host "Upload to $BaseUrl/ :  $name  then  latest.txt   (from $((Resolve-Path $OutDir).Path))"
