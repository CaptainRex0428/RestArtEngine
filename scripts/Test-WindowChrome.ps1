param(
    [string]$Executable = '',
    [string]$CaptureDirectory = 'artifacts/window-chrome-native'
)
$ErrorActionPreference = 'Stop'
$chromeExecutable = $Executable
. (Join-Path $PSScriptRoot 'Test-Capture.ps1') -HelpersOnly
$Executable = $chromeExecutable
$root = Split-Path $PSScriptRoot -Parent
if (!$Executable) { $Executable = Join-Path $root 'build/cmake-docking/Debug/RestArtEngine.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$output = [IO.Path]::GetFullPath((Join-Path $root $CaptureDirectory))
New-Item -ItemType Directory -Force -Path $output | Out-Null
# Isolate layout/theme preferences and any scene edits from the developer's session.
$testProject = Join-Path $output 'Sandbox'
Copy-Item -LiteralPath (Join-Path $root 'Projects/Sandbox') -Destination $output -Recurse -Force
$manifest = Join-Path $testProject 'Sandbox.raproject'
$scene = Join-Path $testProject 'Content/Scenes/Default.rascene'
$sceneHash = (Get-FileHash -LiteralPath $scene).Hash
if (!('ChromeNative' -as [type])) { Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ChromeNative {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Monitor {
        public int Size; public Rect Bounds, Work; public uint Flags;
    }
    public delegate bool EnumCallback(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref Point point);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr monitor, ref Monitor info);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
    public static IntPtr Pack(int x, int y) { return new IntPtr(unchecked((int)((uint)(ushort)x | ((uint)(ushort)y << 16)))); }
}
'@ }
[ChromeNative]::SetProcessDPIAware() | Out-Null
$events = [System.Collections.Generic.List[string]]::new()
function Assert($condition, [string]$message) { if (!$condition) { throw $message } }
function Wait-Until([scriptblock]$condition, [string]$failure) {
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        if (& $condition) { return }
        Start-Sleep -Milliseconds 100
    }
    throw $failure
}
function Client-Bounds {
    $rect = [ChromeNative+Rect]::new()
    $point = [ChromeNative+Point]::new()
    [ChromeNative]::GetClientRect($script:window, [ref]$rect) | Out-Null
    [ChromeNative]::ClientToScreen($script:window, [ref]$point) | Out-Null
    [pscustomobject]@{ X=$point.X; Y=$point.Y; Width=$rect.Right; Height=$rect.Bottom }
}
function Hit([int]$x, [int]$y) {
    $bounds = Client-Bounds
    [ChromeNative]::SendMessage($script:window, 0x84, [IntPtr]::Zero,
        [ChromeNative]::Pack($bounds.X + $x, $bounds.Y + $y)).ToInt32()
}
function Caption-Click([int]$index, [int]$expected) {
    $bounds = Client-Bounds
    $scale = [ChromeNative]::GetDpiForWindow($script:window) / 96.0
    $x = [int]($bounds.Width - (2.5 - $index) * 46 * $scale)
    $y = [int](18 * $scale)
    Assert ((Hit $x $y) -eq $expected) "Caption button $index has the wrong native hit target."
    # Queue both input edges. A synchronous down can reenter an in-progress
    # native maximize after IsZoomed changes, before capture handling settles.
    [ChromeNative]::PostMessage($script:window, 0xA1, [IntPtr]$expected,
        [ChromeNative]::Pack($bounds.X + $x, $bounds.Y + $y)) | Out-Null
    [ChromeNative]::PostMessage($script:window, 0x202, [IntPtr]::Zero, [ChromeNative]::Pack($x, $y)) | Out-Null
}
function Send-Key([int]$key) {
    Focus-CaptureOwner $script:window
    [ChromeNative]::PostMessage($script:window, 0x100, [IntPtr]$key, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 100
    [ChromeNative]::PostMessage($script:window, 0x101, [IntPtr]$key, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
}
function Capture([string]$name) {
    Send-Key 0x78
    Save-CaptureDialog $script:owner (Join-Path $output ($name + '-' + [guid]::NewGuid().ToString('N') + '.bmp'))
}
foreach ($mode in @('editor', 'dirty-close', 'manager')) {
    $arguments = @('--capture-dir', ('"' + $output + '"'))
    if ($mode -ne 'manager') { $arguments += @('--project', ('"' + $manifest + '"')) }
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory $output -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output "$mode-stdout.txt") -RedirectStandardError (Join-Path $output "$mode-stderr.txt")
    $retainedHandle = $process.Handle
    try {
        $script:window = [IntPtr]::Zero
        $script:owner = $process.Id
        Wait-Until {
            [ChromeNative]::EnumWindows({ param($hwnd, $parameter)
                [uint32]$nativeOwner = 0
                [ChromeNative]::GetWindowThreadProcessId($hwnd, [ref]$nativeOwner) | Out-Null
                $className = [Text.StringBuilder]::new(256)
                [ChromeNative]::GetClassName($hwnd, $className, 256) | Out-Null
                if ($nativeOwner -eq $script:owner -and $className.ToString() -in @('RAEngineMaterialLab', 'RAEngineProjectManager')) {
                    $script:window = $hwnd
                }
                return $true
            }, [IntPtr]::Zero) | Out-Null
            $script:window -ne [IntPtr]::Zero
        } 'No application window appeared.'
        Start-Sleep -Seconds 2
        $bounds = Client-Bounds
        $outer = [ChromeNative+Rect]::new()
        [ChromeNative]::GetWindowRect($script:window, [ref]$outer) | Out-Null
        Assert ($bounds.Y -eq $outer.Top -and $bounds.Height -eq $outer.Bottom - $outer.Top) 'Native title bar still consumes client space.'
        $titleHit = Hit 150 18
        Assert ($titleHit -eq 2) "Custom title does not provide HTCAPTION for native dragging (hit=$titleHit, bounds=$bounds)."
        Assert ((Hit 20 18) -eq 3) 'Title icon does not provide the system menu.'
        Assert ((Hit 1 1) -eq 13 -and (Hit ($bounds.Width - 1) ($bounds.Height - 1)) -eq 17) 'Resize corners are missing.'
        if ($mode -eq 'dirty-close') {
            Send-Key 0x2E # Delete the initially selected Actor in the disposable project.
            Caption-Click 2 20
            Start-Sleep -Milliseconds 700
            Assert (!$process.HasExited) 'Caption close discarded dirty editor state without confirmation.'
            Capture 'unsaved-close-dialog'
            Assert ((Get-FileHash -LiteralPath $scene).Hash -eq $sceneHash) 'Close guard unexpectedly saved the scene.'
            $events.Add('PASS: custom close keeps dirty editor alive; scene file unchanged (dialog screenshot captured).')
            continue # Finally terminates only this disposable test process.
        }
        if ($mode -eq 'editor') { Capture 'windowed' }
        Caption-Click 1 9
        Wait-Until { [ChromeNative]::IsZoomed($script:window) } 'Custom maximize button did not maximize.'
        $monitor = [ChromeNative+Monitor]::new()
        $monitor.Size = [Runtime.InteropServices.Marshal]::SizeOf($monitor)
        [ChromeNative]::GetMonitorInfo([ChromeNative]::MonitorFromWindow($script:window, 2), [ref]$monitor) | Out-Null
        $max = Client-Bounds
        Assert ($max.X -eq $monitor.Work.Left -and $max.Y -eq $monitor.Work.Top -and
            $max.Width -eq $monitor.Work.Right - $monitor.Work.Left -and
            $max.Height -eq $monitor.Work.Bottom - $monitor.Work.Top) 'Maximized client does not fit the monitor work area.'
        if ($mode -eq 'editor') { Capture 'maximized' }
        Caption-Click 1 9
        Wait-Until { ![ChromeNative]::IsZoomed($script:window) } 'Custom restore button failed.'
        Caption-Click 0 8
        Wait-Until { [ChromeNative]::IsIconic($script:window) } 'Custom minimize button failed.'
        [ChromeNative]::ShowWindow($script:window, 9) | Out-Null
        Wait-Until { ![ChromeNative]::IsIconic($script:window) } 'Minimized window did not restore.'
        $restored = Client-Bounds
        [ChromeNative]::PostMessage($script:window, 0xA3, [IntPtr]2,
            [ChromeNative]::Pack($restored.X + 150, $restored.Y + 18)) | Out-Null
        Wait-Until { [ChromeNative]::IsZoomed($script:window) } 'Double-click title did not maximize.'
        Caption-Click 1 9
        Wait-Until { ![ChromeNative]::IsZoomed($script:window) } 'Double-click maximize did not restore.'
        $events.Add("PASS: $mode custom frame, resize/caption/system-menu hit targets, minimize, maximize/work-area, restore and title double-click.")
        if ($mode -eq 'editor') {
            Send-Key 0x7A # F11
            Wait-Until { (Hit 150 18) -eq 1 } 'F11 did not disable the custom title bar.'
            $full = Client-Bounds
            Assert ($full.Width -eq $monitor.Bounds.Right - $monitor.Bounds.Left -and
                $full.Height -eq $monitor.Bounds.Bottom - $monitor.Bounds.Top) 'F11 does not cover the monitor.'
            Capture 'fullscreen'
            Send-Key 0x1B # Escape restores instead of closing.
            Wait-Until { (Hit 150 18) -eq 2 } 'Escape did not restore the custom title bar.'
            $after = Client-Bounds
            Assert ($after.Width -eq $bounds.Width -and $after.Height -eq $bounds.Height) 'F11 lost windowed size.'
            [ChromeNative]::SetWindowPos($script:window, [IntPtr]::Zero, 100, 100, 980, 700, 0x14) | Out-Null
            Start-Sleep -Milliseconds 300
            Capture 'resized'
            $events.Add('PASS: F11 monitor coverage, Escape/window-size restoration, resize and real F9 captures.')
        }
        Caption-Click 2 20
        Assert ($process.WaitForExit(10000)) 'Custom close button did not close a clean window.'
        Assert ($null -ne $process.ExitCode -and $process.ExitCode -eq 0) 'Clean close returned an unknown or nonzero exit status.'
        $events.Add("PASS: $mode custom close exits with code 0.")
    } finally {
        if (!$process.HasExited) { Stop-Process -Id $process.Id }
        $process.Dispose()
    }
}
$events | Set-Content -LiteralPath (Join-Path $output 'results.txt') -Encoding utf8
$events
