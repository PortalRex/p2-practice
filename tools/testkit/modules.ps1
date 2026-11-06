# Lists the 32-bit modules of a test copy with their address ranges, so crash-report addresses can be mapped to a DLL.
#   powershell -NoProfile -File modules.ps1 -Job t1 [-Address 0x51421a24,...]
param([Parameter(Mandatory)][string]$Job, [string[]]$Address = @())
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$state = Join-Path $repo "build\test\$Job"
$info = Get-Content (Join-Path $state 'state.json') -Raw | ConvertFrom-Json
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Mods {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("psapi.dll")] public static extern bool EnumProcessModulesEx(IntPtr process, [Out] IntPtr[] modules, int size, out int needed, int filter);
  [DllImport("psapi.dll", CharSet = CharSet.Unicode)] public static extern int GetModuleFileNameEx(IntPtr process, IntPtr module, StringBuilder name, int size);
  [StructLayout(LayoutKind.Sequential)] public struct ModuleInfo { public IntPtr Base; public int Size; public IntPtr Entry; }
  [DllImport("psapi.dll")] public static extern bool GetModuleInformation(IntPtr process, IntPtr module, out ModuleInfo info, int size);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
"@
$list = Join-Path $state 'modules.txt'
$process = [Mods]::OpenProcess(0x0410, $false, $info.pid)
if ($process -ne [IntPtr]::Zero) {
	$handles = New-Object IntPtr[] 1024
	$needed = 0
	[void][Mods]::EnumProcessModulesEx($process, $handles, 1024 * [IntPtr]::Size, [ref]$needed, 1)  # 1 = 32-bit modules
	$lines = for ($i = 0; $i -lt $needed / [IntPtr]::Size; $i++) {
		$name = New-Object Text.StringBuilder 520
		[void][Mods]::GetModuleFileNameEx($process, $handles[$i], $name, 520)
		$mi = New-Object Mods+ModuleInfo
		[void][Mods]::GetModuleInformation($process, $handles[$i], [ref]$mi, [Runtime.InteropServices.Marshal]::SizeOf([type][Mods+ModuleInfo]))
		'{0:x8} {1:x8} {2}' -f $mi.Base.ToInt64(), ($mi.Base.ToInt64() + $mi.Size), [IO.Path]::GetFileName($name.ToString())
	}
	[void][Mods]::CloseHandle($process)
	$lines | Sort-Object | Set-Content $list
	Write-Output "$($lines.Count) modules written to $list"
}
foreach ($a in @($Address | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
	$value = [Convert]::ToInt64($a.Trim(), 16)
	$hit = Get-Content $list | Where-Object { $p = $_ -split ' '; $value -ge [Convert]::ToInt64($p[0], 16) -and $value -lt [Convert]::ToInt64($p[1], 16) }
	if ($hit) { $p = $hit -split ' '; '{0} = {1}+0x{2:x}' -f $a, $p[2], ($value - [Convert]::ToInt64($p[0], 16)) } else { "$a = (no module)" }
}
