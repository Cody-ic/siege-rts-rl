param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('圣城 单文件验收 ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$copy = Join-Path $testRoot '圣城 Portable.exe'
Copy-Item -LiteralPath $Executable -Destination $copy
$oldLocalAppData = $env:LOCALAPPDATA
try {
    # Isolate test saves and logs from the player's real campaigns.
    $env:LOCALAPPDATA = Join-Path $testRoot '用户数据'
    New-Item -ItemType Directory -Path $env:LOCALAPPDATA | Out-Null
    $process = Start-Process -FilePath $copy -WorkingDirectory $testRoot -ArgumentList '--verify-assets' -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Asset verification failed: exit $($process.ExitCode). Logs: $testRoot" }
    $log = Join-Path $env:LOCALAPPDATA 'SiegeRTS\launcher.log'
    if (-not (Test-Path -LiteralPath $log)) { throw "Game diagnostic log was not created: $log" }
    $remaining = Join-Path $env:LOCALAPPDATA 'SiegeRTS\portable'
    if ((Test-Path -LiteralPath $remaining) -and @(Get-ChildItem -LiteralPath $remaining -Directory).Count -ne 0) {
        throw "Asset check completed, but extraction directories remain: $remaining"
    }
    $sentinel = Join-Path $env:LOCALAPPDATA 'SiegeRTS\preservation-check.txt'
    Set-Content -LiteralPath $sentinel -Value 'preserve user data' -Encoding ASCII
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
    $checks = @(
        ('--menu --size 1280 720 --screenshot "' + (Join-Path $testRoot 'menu.png') + '"')
        ('--strategy-smoke models/attacker.onnx --size 1280 720 --screenshot "' + (Join-Path $testRoot 'strategy.png') + '"')
        ('--battle --rl-policy models/attacker.onnx --ticks 1800 --size 1280 720 --screenshot "' + (Join-Path $testRoot 'battle.png') + '"')
    )
    foreach ($arguments in $checks) {
        $process = Start-Process -FilePath $copy -WorkingDirectory $testRoot -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
        if ($process.ExitCode -ne 0) { throw "Game check failed: $arguments" }
        if (@(Get-ChildItem -LiteralPath $remaining -Directory).Count -ne 0) { throw 'Temporary extraction was not cleaned.' }
        if ((Get-FileHash -LiteralPath $sentinel).Hash -ne $sentinelHash) { throw 'User data preservation failed.' }
    }
    Write-Host "Assets, menu, RL roundtrip, battle, data preservation and temporary cleanup passed. Evidence: $testRoot"
    Write-Host 'Menu, combat, save/resume, and script/RL switching still require manual Windows acceptance.'
}
finally { $env:LOCALAPPDATA = $oldLocalAppData }
