# Skyline - Copyright (C) 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
# Creates the release signing key. Run ONCE, on your own PC:
#   powershell -ExecutionPolicy Bypass -File tools\new-signing-key.ps1
#
# - The private key goes to %USERPROFILE%\skyline-signing.key. Keep it secret and
#   backed up, never put it in this folder or on the server. Share it with your
#   co-developer privately (e.g. USB stick). Losing it means existing installs
#   can't be updated anymore; anyone who has it can push code to every user.
# - The public key is written into src\update_key.h, which gets built into Skyline.exe.
param(
    [string]$KeyFile = "$env:USERPROFILE\skyline-signing.key"
)
$ErrorActionPreference = 'Stop'
$header = Join-Path $PSScriptRoot '..\src\update_key.h'

if (Test-Path $KeyFile) {
    throw "$KeyFile already exists. Making a new key would lock out every installed Skyline. Delete it yourself first if you really mean it."
}

$params = New-Object System.Security.Cryptography.CngKeyCreationParameters
$params.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
$key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, $null, $params)
$private = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$public  = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
[System.IO.File]::WriteAllBytes($KeyFile, $private)

$rows = for ($i = 0; $i -lt $public.Length; $i += 12) {
    '    ' + (($public[$i..([Math]::Min($i + 11, $public.Length - 1))] | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', ') + ','
}
$text = @"
// Skyline - Copyright (C) 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Public half of the release signing key (BCRYPT_ECCPUBLIC_BLOB, ECDSA P-256).
// Written by tools/new-signing-key.ps1. All zeros = no key yet, and this build
// never updates itself.
#pragma once

static const unsigned char kUpdateKey[72] = {
$($rows -join "`n")
};
"@
[System.IO.File]::WriteAllText((Resolve-Path $header), $text.Replace("`r`n", "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Private key: $KeyFile  (keep it secret, back it up)"
Write-Host "Public key written to src\update_key.h. Build Skyline, then sign releases with tools\sign-release.ps1."
