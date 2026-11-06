# Ends a test copy started by launch.ps1: unmutes it (Windows remembers per-app mute), asks it to quit, kills only
# its own PID if it doesn't, saves its console log and any listed files, then deletes its profile.
#
#   powershell -NoProfile -File stop.ps1 -Job t1 [-Keep "*.dem","*.mp4"]
param([Parameter(Mandatory)][string]$Job, [string[]]$Keep = @())
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$state = Join-Path $repo "build\test\$Job"
$info = Get-Content (Join-Path $state 'state.json') -Raw | ConvertFrom-Json
$proc = Get-Process -Id $info.pid -ErrorAction SilentlyContinue
if ($proc -and $proc.ProcessName -eq 'portal2') {
	& 'H:\Claude\p2ce-re\test\mute.ps1' -ProcessId $info.pid -Unmute | Out-Null
	& python (Join-Path $PSScriptRoot 'netcon.py') --port $info.port --password $info.password --quiet 'quit' 2>$null | Out-Null
	if (-not $proc.WaitForExit(8000)) { Stop-Process -Id $info.pid -Force; [void]$proc.WaitForExit(5000) }
	Write-Output "stopped pid $($info.pid)"
} else {
	Write-Output "pid $($info.pid) already gone"
}

$profile = $info.profile
if (Test-Path $profile) {
	foreach ($f in @('console.log') + $Keep) {
		Get-ChildItem -Path $profile -Filter $f -File -Recurse -ErrorAction SilentlyContinue |
			Where-Object { $_.FullName -notlike '*\maps\workshop\*' } |
			ForEach-Object { Copy-Item $_.FullName (Join-Path $state $_.Name) -Force }
	}
}
# Only ever this job's folder under p2practice-test. The workshop maps in it are hard links: removing them
# leaves the game's own files alone.
$root = $info.root
if ($root -and $root -like '*\p2practice-test\*' -and (Test-Path $root)) {
	for ($i = 0; $i -lt 20 -and (Test-Path $root); $i++) {
		Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
		if (Test-Path $root) { Start-Sleep -Milliseconds 500 }
	}
	$parent = Split-Path -Parent $root
	if ((Test-Path $parent) -and -not (Get-ChildItem $parent -Force)) { Remove-Item -LiteralPath $parent -Force }
	Write-Output ("profile removed: " + (-not (Test-Path $root)))
}
