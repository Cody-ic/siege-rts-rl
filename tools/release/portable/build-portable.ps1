param(
    [string]$InputZip,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist')
)
$ErrorActionPreference = 'Stop'
$expected = '2de9442252d3f5f741f2f002f4e376ae926c07451a4c8186782716356e07510a'
$url = 'https://github.com/Cody-ic/siege-rts-rl/releases/download/v1.0.1/Sanctum-1.0.1-Windows-x64.zip'
if (-not [Environment]::Is64BitOperatingSystem) { throw 'Windows x64 is required.' }
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $csc)) { throw 'The Windows .NET Framework C# compiler was not found.' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$exe = Join-Path $OutputDirectory 'Sanctum-1.0.1-Portable-Windows-x64.exe'
if (Test-Path -LiteralPath $exe) { throw "Refusing to overwrite: $exe" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stage = Join-Path ([IO.Path]::GetTempPath()) ('sanctum-build-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $payload = Join-Path $stage 'payload.zip'
    if ($InputZip) { Copy-Item -LiteralPath $InputZip -Destination $payload }
    else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $payload
    }
    if ((Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        throw 'The input ZIP does not match the official v1.0.1 SHA256. Build stopped.'
    }
    $source = Join-Path $PSScriptRoot 'PortableLauncher.cs'
    $manifest = Join-Path $PSScriptRoot 'portable.manifest'
    $tempExe = Join-Path $stage 'Sanctum-1.0.1-Portable-Windows-x64.exe'
    $arguments = @('/nologo','/target:winexe','/platform:x64','/optimize+','/warnaserror+',
        '/reference:System.dll','/reference:System.Core.dll','/reference:System.Windows.Forms.dll',
        '/reference:System.IO.Compression.dll',('/win32manifest:' + $manifest),
        ('/resource:' + $payload + ',payload.zip'),('/out:' + $tempExe),$source)
    & $csc @arguments
    if ($LASTEXITCODE -ne 0) { throw "C# build failed with exit code $LASTEXITCODE" }
    Copy-Item -LiteralPath $tempExe -Destination $exe
    $hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ASCII -Value ($hash + '  ' + [IO.Path]::GetFileName($exe))
    Write-Host "Built: $exe"
    Write-Host 'This is a build result, not a Windows acceptance result. Complete the README checks before publishing.'
}
finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    if (-not $resolvedStage.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedStage) -notmatch '^sanctum-build-[0-9a-f]{32}$') {
        throw 'Refusing to clean an unexpected staging path.'
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
