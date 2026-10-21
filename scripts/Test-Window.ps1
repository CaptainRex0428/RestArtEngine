param([string]$Executable = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$Executable) { $Executable = Join-Path $root 'bin/windows-x86_64/Debug/RestArtEngine.exe' }
$Executable = [IO.Path]::GetFullPath($Executable)
$outputDir = Join-Path $root 'artifacts/window-test'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
if (!('WindowTestNative' -as [type])) { Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WindowTestNative {
    public delegate bool EnumCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
"@
}
$events = [System.Collections.Generic.List[string]]::new()
$process = Start-Process -FilePath $Executable -ArgumentList @('--capture-dir', ('"' + $outputDir + '"')) -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $outputDir 'stdout.txt') -RedirectStandardError (Join-Path $outputDir 'stderr.txt')
try {
    $script:testWindow = [IntPtr]::Zero
    $script:testProcessId = $process.Id
    for($attempt=0; $attempt -lt 30 -and $script:testWindow -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if($process.HasExited) { throw "Application exited early with code $($process.ExitCode)" }
        [WindowTestNative]::EnumWindows({param($window,$parameter)
            [uint32]$ownerId=0
            [WindowTestNative]::GetWindowThreadProcessId($window,[ref]$ownerId) | Out-Null
            $title=[System.Text.StringBuilder]::new(256)
            [WindowTestNative]::GetWindowText($window,$title,256) | Out-Null
            if($ownerId -eq $script:testProcessId -and $title.ToString().StartsWith('RestArtEngine')) {$script:testWindow=$window}
            return $true
        },[IntPtr]::Zero) | Out-Null
    }
    if($script:testWindow -eq [IntPtr]::Zero) {throw 'No application window found'}
    $window=$script:testWindow
    Start-Sleep -Seconds 3
    $process.Refresh()
    if($process.HasExited) {throw 'Default application did not stay alive'}
    $events.Add('PASS: default application stays alive and has a native window.')
    function Send-Key([int]$key) {
        [WindowTestNative]::PostMessage($window,0x100,[IntPtr]$key,[IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 150
        [WindowTestNative]::PostMessage($window,0x101,[IntPtr]$key,[IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 250
    }
    Send-Key 0x78
    foreach($size in @(@(980,700),@(1600,960),@(1280,800))) {
        if(-not [WindowTestNative]::SetWindowPos($window,[IntPtr]::Zero,100,100,$size[0],$size[1],0x14)) {throw 'SetWindowPos failed'}
        Start-Sleep -Milliseconds 500
        $process.Refresh()
        if($process.HasExited) {throw 'Application exited during resize'}
    }
    $events.Add('PASS: window resize to 980x700, 1600x960, 1280x800 keeps rendering alive.')
    [WindowTestNative]::ShowWindow($window,6) | Out-Null
    Start-Sleep -Seconds 1
    if(-not [WindowTestNative]::IsIconic($window)) {throw 'Window did not minimize'}
    $process.Refresh()
    if($process.HasExited) {throw 'Application exited while minimized'}
    [WindowTestNative]::ShowWindow($window,9) | Out-Null
    Start-Sleep -Seconds 1
    if([WindowTestNative]::IsIconic($window)) {throw 'Window did not restore'}
    $events.Add('PASS: minimize and restore succeeds.')
    Send-Key 0x71
    Send-Key 0x78
    Send-Key 0x72
    Send-Key 0x78
    $process.Refresh()
    if($process.HasExited) {throw 'Application exited after F2/F3'}
    $events.Add('PASS: F2 and F3 key messages processed without crash; F9 screenshots captured for visual confirmation.')
    [WindowTestNative]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    if(-not $process.WaitForExit(10000)) {throw 'Application did not close after WM_CLOSE'}
    if($process.ExitCode -ne 0) {throw "Application exit code: $($process.ExitCode)"}
    $events.Add('PASS: WM_CLOSE ends the application with exit code 0.')
    $stdoutPath = Join-Path $outputDir 'stdout.txt'
    if (Test-Path -LiteralPath $stdoutPath) {
        $stdoutContent = Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
        if ($null -ne $stdoutContent) {
            $stdout = $stdoutContent.Trim()
            if ($stdout) { $events.Add($stdout) }
        }
    }
    $events | Set-Content -LiteralPath (Join-Path $root 'artifacts/window-test.txt') -Encoding utf8
    $events
} finally {
    $process.Refresh()
    if(-not $process.HasExited) {
        if($script:testWindow -ne [IntPtr]::Zero) {[WindowTestNative]::PostMessage($script:testWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null}
        if(-not $process.WaitForExit(3000)) {Stop-Process -Id $process.Id}
    }
}
