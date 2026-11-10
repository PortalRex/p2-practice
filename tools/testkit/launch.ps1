# Starts a hidden, muted, low-priority test copy of Portal 2 beside whatever the player is running, and keeps it
# polite until it exits. Development tool only (the launcher users get starts the game normally).
#
#   powershell -NoProfile -File launch.ps1 -Job t1 -Port 2141 [-Plugins a.dll,b.dll] [-Sar] [-Cfg "line","line"]
#       [-GameArgs "+map x"] [-Width 1280] [-Height 720] [-Inject some.dll]
# -Inject loads a DLL into the game before any of its code runs (ChocolateLoading's way: its chocoload.exe does
# the same), with CHOCOLOAD_EARLY=1 in the game's environment.
#
# The method is Portal 2 Demo Splicer's (its GameLauncher, NoFocus and AudioMute classes are called from its exe):
# own profile, VPROJECT + -allowmultiple, focus/EnumWindows calls patched out, helper window untagged, mixer mute,
# below-normal CPU/GPU/memory priority. Sends no input. Blocks until the game exits, so run it in the background;
# state is written to build\test\<job>\state.json.
param(
	[Parameter(Mandatory)][string]$Job,
	[int]$Port = 2141,
	[string]$Password = 'p2practice',
	[string[]]$Plugins = @(),
	[switch]$Sar,
	[string[]]$Cfg = @(),
	[string]$GameArgs = '',
	[switch]$CopyOnly,          # copy -Plugins into the profile without loading them from the autoexec
	[string[]]$CfgFiles = @(),  # extra cfg files copied into the profile's cfg folder
	[int]$Width = 1280,
	[int]$Height = 720,
	[string]$Inject = '',
	[switch]$Sound             # leave the game's sound on (to listen to a change); the game is muted otherwise
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# (Portal 2 Demo Splicer's exe holds the hidden-launch code; P2_SPLICER points at another copy)
$splicer = if ($env:P2_SPLICER) { $env:P2_SPLICER } else { 'H:\Claude\p2-demo-splicer\bin\P2DemoSplicer.exe' }
$asm = [Reflection.Assembly]::LoadFrom($splicer)
$static = [Reflection.BindingFlags]'Public,NonPublic,Static'
function Splicer($type, $method) { $asm.GetType("P2DemoSplicer.$type").GetMethod($method, $static) }

[string]$game = (Splicer GameLocator Find).Invoke($null, @())
if (-not $game) { throw 'Portal 2 not found' }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running' }

[string]$root = Join-Path $game "p2practice-test\$Job"
[string]$profile = Join-Path $root 'portal2'
[string]$state = Join-Path $repo "build\test\$Job"
[string]$stateFile = Join-Path $state 'state.json'

# A state.json left by an earlier run names that run's PID. If that copy is still running the job is in use;
# otherwise the file goes now, so that a failure below can't leave stop.ps1 an old PID to act on.
if (Test-Path $stateFile) {
	$old = $null
	try { $old = Get-Content $stateFile -Raw | ConvertFrom-Json } catch { }
	if ($old -and $old.pid) {
		$oldCmd = (Get-CimInstance Win32_Process -Filter "ProcessId = $($old.pid) AND Name = 'portal2.exe'" -ErrorAction SilentlyContinue).CommandLine
		if ($oldCmd -and $oldCmd.IndexOf("p2practice-test\$Job\portal2`"", [StringComparison]::OrdinalIgnoreCase) -ge 0) {
			throw "job '$Job' is still running as pid $($old.pid): end it first (stop.ps1 -Job $Job)"
		}
	}
	Remove-Item -LiteralPath $stateFile -Force
}

[Diagnostics.Process]$proc = $null
try {
New-Item -ItemType Directory -Force -Path (Join-Path $profile 'cfg'), $state | Out-Null

# The splicer's gameinfo: this folder first, then the game's own content
(Splicer RenderProfile GameInfoFor).Invoke($null, [object[]]@($profile, $null)) | Set-Content -Encoding ascii (Join-Path $profile 'gameinfo.txt')

$autoexec = @(
	'// p2practice test copy',
	'voice_enable 0',     # co-op maps otherwise keep opening the microphone
	'cl_mouseenable 0',   # never touch the cursor
	'engine_no_focus_sleep 0',  # run at full speed without focus
	'sv_lan 1',           # a second copy on one Steam account is dropped ("No Steam logon") unless the server is LAN
	'gameinstructor_enable 0',
	'sv_gameinstructor_disable 1'
)
# Plugins load from the autoexec: the engine honours only one +plugin_load on the command line (a second one
# loads the first plugin again)
$load = @()
foreach ($dll in @($Plugins | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
	if (-not $CopyOnly) { $load += 'plugin_load ' + [IO.Path]::GetFileNameWithoutExtension($dll) }
}
if ($Sar) {
	Copy-Item (Join-Path $game 'sar.dll') (Join-Path $profile 'sar.dll')
	$load += 'plugin_load sar'
	$load += 'sar_disable_no_focus_sleep 1'  # (after the load: the setting doesn't exist before it)
}
# -File passes "a,b" as one string: split lists back into items
$Plugins = @($Plugins | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Cfg = @($Cfg | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($dll in $Plugins) { Copy-Item $dll $profile }
foreach ($cfg in @($CfgFiles | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })) { Copy-Item $cfg (Join-Path $profile 'cfg') }
($autoexec + $load + $Cfg) | Set-Content -Encoding ascii (Join-Path $profile 'cfg\autoexec.cfg')

# Without the workshop maps linked in, the menu downloads every subscribed map into the profile
(Splicer RenderProfile LinkWorkshopMaps).Invoke($null, [object[]]@($game, $profile))

# The plugin's development trace (see engine.cpp) goes next to the launcher state
$env:P2PRACTICE_LOG = Join-Path $state 'trace.log'
$gameArg = "p2practice-test\$Job\portal2"
$cmd = "-game `"$gameArg`" -allowmultiple -novid -condebug -windowed -w $Width -h $Height -netconport $Port -netconpassword $Password " + $GameArgs
if ($Inject) { $env:CHOCOLOAD_EARLY = '1' }
[object[]]$launchArgs = @([string](Join-Path $game 'portal2.exe'), [string]$cmd, $game, $false, [string]$Inject)
$start = $asm.GetType('P2DemoSplicer.GameLauncher').GetMethods($static) | Where-Object { $_.Name -eq 'StartInBackground' -and $_.GetParameters().Count -eq 5 }
$proc = $start.Invoke($null, $launchArgs)
$focusBlocked = [bool]$launchArgs[3]

# (startTimeUtc is how stop.ps1 tells this process from a later one that was given the same PID)
$info = [ordered]@{ job = $Job; pid = $proc.Id; port = $Port; password = $Password; profile = $profile; root = $root
	focusBlocked = $focusBlocked; muted = $false; gpuLowered = $false; started = (Get-Date).ToString('s')
	startTimeUtc = $proc.StartTime.ToUniversalTime().ToString('o'); exited = $null }
function Save { $info | ConvertTo-Json | Set-Content -Encoding utf8 $stateFile }
Save
} catch {
	# Nothing is left for a later run or stop.ps1 to trip over: a game that did start is this script's own child
	# and is ended, then the half-made profile goes (only ever this job's folder under p2practice-test).
	if ($proc -and -not $proc.HasExited) { try { $proc.Kill(); [void]$proc.WaitForExit(5000) } catch { } }
	Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
	if ($root -like '*\p2practice-test\*') {
		for ($i = 0; $i -lt 10 -and (Test-Path $root); $i++) {
			Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
			if (Test-Path $root) { Start-Sleep -Milliseconds 500 }
		}
	}
	throw
}
Write-Output "started pid $($proc.Id), netcon port $Port, focus blocked: $focusBlocked"

# HideFromOtherGames loops until the game exits and LowerGpuPriority until it takes, so both run on pool threads
# while this thread mutes the game's audio session as soon as it exists.
$hide = [Delegate]::CreateDelegate([Func[Diagnostics.Process, int]], (Splicer GameLauncher HideFromOtherGames))
$gpu = [Delegate]::CreateDelegate([Func[Diagnostics.Process, bool]], (Splicer GameLauncher LowerGpuPriority))
$hiding = $hide.BeginInvoke($proc, $null, $null)
$lowering = $gpu.BeginInvoke($proc, $null, $null)
$mute = Splicer AudioMute Mute
while (-not $proc.HasExited) {
	$changed = $false
	if (-not $Sound -and -not $info.muted) {
		try { if ($mute.Invoke($null, [object[]]@([uint32]$proc.Id))) { $info.muted = $true; $changed = $true } }
		catch { $info.muteError = "$($_.Exception.InnerException.Message)" }
	}
	if (-not $info.gpuLowered -and $lowering.IsCompleted) { $info.gpuLowered = [bool]$gpu.EndInvoke($lowering); $changed = $true; $lowering = $null }
	if ($changed) { Save }
	Start-Sleep -Milliseconds 100
}
$info.helperTagsCleared = $hide.EndInvoke($hiding)
$info.exited = (Get-Date).ToString('s')
$info.exitCode = $proc.ExitCode
Save
Write-Output "game exited with code $($proc.ExitCode)"
