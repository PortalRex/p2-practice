# Packs a release: dist\P2Practice-<version>.zip with the launcher, the plugin, p2route.exe and ChocolateLoading
# (for save states), ready to unzip anywhere. Build first: cmake --build build\x86 --config Release; launcher\build.bat
#   powershell -NoProfile -File tools\package.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path (Join-Path $repo 'plugin\src\version.h') -Pattern '"([0-9.]+)"').Matches[0].Groups[1].Value
$bin = Join-Path $repo 'launcher\bin'
$stage = Join-Path $repo "dist\P2Practice-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'chocolateloading') | Out-Null

# the launcher, and the plugin and route reader it runs (fresh builds, not whatever sits in launcher\bin)
Copy-Item (Join-Path $bin 'P2Practice.exe') $stage
Copy-Item (Join-Path $repo 'build\x86\plugin\Release\p2practice.dll') $stage
Copy-Item (Join-Path $repo 'build\x86\tools\routeinfo\Release\p2route.exe') $stage
# ChocolateLoading (Blenderiste09, redistributed with permission), with the Steam-overlay fix
foreach ($f in 'chocoload.exe', 'chocolateloading.dll') { Copy-Item (Join-Path $bin "chocolateloading\$f") (Join-Path $stage 'chocolateloading') }
Copy-Item (Join-Path $repo 'third_party\chocolateloading\README.txt') (Join-Path $stage 'chocolateloading')
Copy-Item (Join-Path $repo 'third_party\chocolateloading\fixes.patch') (Join-Path $stage 'chocolateloading')
Copy-Item (Join-Path $repo 'README.md') $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'demos') | Out-Null

$zip = Join-Path $repo "dist\P2Practice-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Get-Item $zip | Select-Object FullName, Length
