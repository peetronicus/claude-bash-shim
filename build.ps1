# Builds dist\bash.exe with clang, or with cl when run from a Visual Studio developer prompt.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root 'src\bash.c'
$dist = Join-Path $root 'dist'
$output = Join-Path $dist 'bash.exe'
New-Item -ItemType Directory -Force $dist | Out-Null

$clang = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clang) {
    $default = Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe'
    if (Test-Path $default) { $clang = Get-Command $default }
}
$cl = Get-Command cl -ErrorAction SilentlyContinue

if ($clang) {
    & $clang.Source -O2 -Wall -Wextra -fms-runtime-lib=static $source -o $output -lshell32 -ladvapi32
} elseif ($cl) {
    Push-Location $dist
    try { & $cl.Source /nologo /O2 /W4 /MT $source /Fe:$output shell32.lib advapi32.lib } finally { Pop-Location }
} else {
    throw 'No compiler found. Install LLVM (winget install LLVM.LLVM) or run this from a Visual Studio developer prompt.'
}
if ($LASTEXITCODE -ne 0) { throw "The compiler exited with $LASTEXITCODE." }
Write-Host "Built $output"
