# Ends a test copy started by launch.ps1: unmutes it (Windows remembers per-app mute), asks it to quit, kills only
# its own PID if it doesn't, saves its console log and any listed files, then deletes its profile.
#
#   powershell -NoProfile -File stop.ps1 -Job t1 [-Keep "*.dem","*.mp4"]
param([Parameter(Mandatory)][string]$Job, [string[]]$Keep = @())
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$state = Join-Path $repo "build\test\$Job"
$stateFile = Join-Path $state 'state.json'
if (-not (Test-Path $stateFile)) { Write-Output "no state.json for job '$Job' in ${state}: nothing to stop"; exit 1 }
$info = Get-Content $stateFile -Raw | ConvertFrom-Json

# The PID in state.json may be stale and reused by now, perhaps by the player's own game. It is the test copy only
# when it is a portal2.exe whose command line names this job's profile and, where launch.ps1 recorded one, whose
# start time is the recorded one. Anything else is left alone.
function Test-IsTestCopy($proc) {
	if (-not $proc -or $proc.ProcessName -ne 'portal2') { return $false }
	$wmi = Get-CimInstance Win32_Process -Filter "ProcessId = $($proc.Id)" -ErrorAction SilentlyContinue
	if (-not $wmi -or -not $wmi.CommandLine) { return $false }
	if ($wmi.CommandLine.IndexOf("p2practice-test\$Job\portal2`"", [StringComparison]::OrdinalIgnoreCase) -lt 0) { return $false }
	if ($info.startTimeUtc) {
		try {
			$started = $proc.StartTime.ToUniversalTime()
			$recorded = [DateTime]::Parse([string]$info.startTimeUtc, [Globalization.CultureInfo]::InvariantCulture,
				[Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
		} catch { return $false }
		if ([Math]::Abs(($started - $recorded).TotalSeconds) -gt 2) { return $false }
	}
	return $true
}

$proc = if ($info.pid) { Get-Process -Id $info.pid -ErrorAction SilentlyContinue } else { $null }
if (-not $proc) {
	Write-Output "pid $($info.pid) already gone"
} elseif (Test-IsTestCopy $proc) {
	# (the mixer helper belongs to another project: without it the copy is stopped all the same)
	$muteScript = 'H:\Claude\p2ce-re\test\mute.ps1'
	if (Test-Path $muteScript) { & $muteScript -ProcessId $info.pid -Unmute | Out-Null }
	else { Write-Warning "$muteScript not found: portal2.exe may stay muted in the Windows volume mixer, unmute it there by hand" }
	& python (Join-Path $PSScriptRoot 'netcon.py') --port $info.port --password $info.password --quiet 'quit' 2>$null | Out-Null
	if (-not $proc.WaitForExit(8000)) { Stop-Process -Id $info.pid -Force; [void]$proc.WaitForExit(5000) }
	Write-Output "stopped pid $($info.pid)"
} else {
	Write-Warning "pid $($info.pid) is not job '$Job''s test copy (stale state.json, the PID was reused): leaving it and the profile alone"
	exit 1
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
