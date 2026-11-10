# Opens dist/SanctuaryPort-windows.zip as a player gets it, on a Windows without MSYS2: no game or
# player file ships, sanctuary-probe.exe finds every DLL it links, the package's Python imports what
# the preparation scripts need and the launcher's window stays open.
# Usage: powershell -File smoke-test.ps1 path\to\SanctuaryPort-windows.zip
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference = 'Stop'
# A player's PATH: a DLL found only in MSYS2 or another toolchain must not hide a missing one.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$work = Join-Path ([IO.Path]::GetTempPath()) ("sanctuary-smoke-" + [guid]::NewGuid())
Expand-Archive -LiteralPath $Package -DestinationPath $work
$tree = Join-Path $work 'SanctuaryPort-windows'
$failed = @()

# The same names package.sh refuses to ship: game files and anything a player makes.
$forbidden = 'eboot.bin', 'sce_module', 'sce_sys', 'savedata', 'user', 'cache', '*.log', 'sanctuary.ini',
    'settings.json', 'mods.json', 'patches.json', 'fsr4_shaders', 'nvngx_dlss*.dll', '*.spv', '*.pkg',
    '*.sprx', '*.prx'
$found = Get-ChildItem -LiteralPath $tree -Recurse -Force |
    Where-Object { $name = $_.Name; $forbidden | Where-Object { $name -like $_ } }
if ($found) { $failed += "files that must not ship: $($found.FullName -join ', ')" }

# A missing DLL stops the probe before main (0xC0000135).
$probe = Start-Process -FilePath (Join-Path $tree 'bin\sanctuary-probe.exe') -ArgumentList '--cpu-features' `
    -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $work 'probe.log')
$null = $probe.Handle  # without it ExitCode stays empty after WaitForExit(timeout)
if (-not $probe.WaitForExit(60000)) {
    Stop-Process -Id $probe.Id -Force
    $probe.WaitForExit()
    $failed += 'sanctuary-probe.exe --cpu-features did not finish in 60 s'
} elseif ($probe.ExitCode -ne 0) {
    $failed += ('sanctuary-probe.exe --cpu-features exited 0x{0:X}' -f $probe.ExitCode)
}
Get-Content (Join-Path $work 'probe.log')

$launcher = Join-Path $tree 'SanctuaryPort.exe'
$python = Join-Path $tree 'python\python.exe'
$check = Join-Path $work 'imports.py'
Set-Content -LiteralPath $check -Value @'
import sys, traceback
try:
    import PIL.Image, iced_x86
except Exception:
    traceback.print_exc()
    sys.exit(1)
print('imports: PIL', PIL.__version__, 'iced_x86')
'@
$script = Start-Process -FilePath $python -ArgumentList "`"$check`"" -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $work 'script.log') -RedirectStandardError (Join-Path $work 'script.err')
$null = $script.Handle
if (-not $script.WaitForExit(60000)) {
    Stop-Process -Id $script.Id -Force
    $script.WaitForExit()
    $failed += 'python\python.exe did not finish in 60 s'
} elseif ($script.ExitCode -ne 0) {
    $failed += "python\python.exe could not import PIL or iced_x86 ($($script.ExitCode))"
}
Get-Content (Join-Path $work 'script.log'), (Join-Path $work 'script.err')

$window = Start-Process -FilePath $launcher -PassThru
Start-Sleep -Seconds 15
if ($window.HasExited) {
    $failed += "the launcher exited ($($window.ExitCode)) instead of staying open"
} else {
    Stop-Process -Id $window.Id -Force
    $window.WaitForExit()
}

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($failed) {
    $failed | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host 'OK: the package opens without MSYS2'
