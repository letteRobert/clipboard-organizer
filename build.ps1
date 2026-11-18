param([string]$ZigPath)
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    if (-not $ZigPath) {
        $localZig = Join-Path $PSScriptRoot '.native-build\ziglang\zig.exe'
        $ZigPath = if (Test-Path -LiteralPath $localZig) { $localZig } else { (Get-Command zig -ErrorAction Stop).Source }
    }
    New-Item -ItemType Directory -Force .native-build,app | Out-Null
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot '.native-build\global-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $PSScriptRoot '.native-build\cache'
    & $ZigPath rc /nologo /fo .native-build/app.res src/app.rc
    if ($LASTEXITCODE) { throw 'Resource compilation failed.' }
    & $ZigPath c++ -std=c++20 -Os -s -DUNICODE -D_UNICODE -municode '-Wl,--subsystem,windows' src/main.cpp .native-build/app.res -o app/ClipboardHistory.exe -lcomctl32 -lshell32 -lgdi32 -luser32 -lkernel32
    if ($LASTEXITCODE) { throw 'C++ compilation failed.' }
    $test = Start-Process -FilePath (Join-Path $PSScriptRoot 'app\ClipboardHistory.exe') -ArgumentList '--self-test' -Wait -PassThru -WindowStyle Hidden
    if ($test.ExitCode) { throw "History compatibility test failed ($($test.ExitCode))." }
    Get-Item -LiteralPath 'app\ClipboardHistory.exe' | Select-Object Name,Length
} finally {
    Pop-Location
}
