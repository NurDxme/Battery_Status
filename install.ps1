# Baut Battery Status als Release, installiert es nach
# %LOCALAPPDATA%\Programs\BatteryStatus und richtet den Autostart ein.
#
# Aufruf:   powershell -ExecutionPolicy Bypass -File install.ps1
# Entfernen: powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall

param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\BatteryStatus'
$Exe        = Join-Path $InstallDir 'Battery_Status.exe'
$RunKey     = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$RunName    = 'BatteryStatus'

# Laufende Instanz beenden (sonst kann die .exe nicht ersetzt werden)
Get-Process Battery_Status -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if ($Uninstall) {
    Remove-ItemProperty -Path $RunKey -Name $RunName -ErrorAction SilentlyContinue
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host 'Battery Status wurde entfernt.'
    return
}

# CLion-Toolchain suchen (neueste Version)
$clion = Get-ChildItem 'C:\Program Files\JetBrains' -Directory -Filter 'CLion*' |
         Sort-Object Name -Descending | Select-Object -First 1
if (-not $clion) { throw 'CLion wurde nicht gefunden.' }
$bin   = Join-Path $clion.FullName 'bin'
$cmake = Join-Path $bin 'cmake\win\x64\bin\cmake.exe'
$env:PATH = "$bin\mingw\bin;$bin\ninja\win\x64;$env:PATH"

# Release bauen
$src   = $PSScriptRoot
$build = Join-Path $src 'cmake-build-release'
& $cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
         "-DCMAKE_C_COMPILER=$bin/mingw/bin/gcc.exe" "-DCMAKE_CXX_COMPILER=$bin/mingw/bin/c++.exe"
if ($LASTEXITCODE) { throw 'CMake-Konfiguration fehlgeschlagen.' }
& $cmake --build $build
if ($LASTEXITCODE) { throw 'Build fehlgeschlagen.' }

# Installieren
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item (Join-Path $build 'Battery_Status.exe')   $InstallDir -Force
Copy-Item (Join-Path $build 'libwinpthread-1.dll')  $InstallDir -Force

# Autostart
Set-ItemProperty -Path $RunKey -Name $RunName -Value "`"$Exe`""

Start-Process $Exe
Write-Host "Installiert nach $InstallDir - startet ab jetzt automatisch mit Windows."
