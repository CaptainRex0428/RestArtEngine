param(
    [string]$Executable = '',
    [string]$OutputDirectory = '',
    [switch]$HelpersOnly
)
$ErrorActionPreference = 'Stop'
if (!('CaptureNative' -as [type])) { Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CaptureNative {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    public delegate bool EnumCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint first, uint second, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, string lp);
    public static IntPtr Find(uint pid, string className, string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner != pid) return true;
            var cls = new StringBuilder(256); GetClassName(window, cls, cls.Capacity);
            var name = new StringBuilder(1024); GetWindowText(window, name, name.Capacity);
            if ((className != "#32770" || IsWindowVisible(window)) && cls.ToString() == className && (String.IsNullOrEmpty(title) || name.ToString() == title)) found = window;
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindConfirmation(uint pid, IntPtr excluded) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            var cls = new StringBuilder(256); GetClassName(window, cls, cls.Capacity);
            if (owner == pid && window != excluded && cls.ToString() == "#32770" && IsWindowEnabled(window) && IsWindowVisible(window)) found = window;
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FilenameEdit(IntPtr parent) {
        IntPtr result = IntPtr.Zero;
        EnumChildWindows(parent, (window, parameter) => {
            var cls = new StringBuilder(256); GetClassName(window, cls, cls.Capacity);
            if (IsWindowVisible(window) && cls.ToString() == "Edit" && (GetDlgCtrlID(window) == 1001 || GetDlgCtrlID(window) == 0x480)) result = window;
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static IntPtr Button(IntPtr parent, int id) {
        IntPtr result = IntPtr.Zero;
        EnumChildWindows(parent, (window, parameter) => {
            var cls = new StringBuilder(256); GetClassName(window, cls, cls.Capacity);
            if (IsWindowVisible(window) && cls.ToString() == "Button" && GetDlgCtrlID(window) == id) result = window;
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static IntPtr Pack(int x, int y) { return new IntPtr(unchecked((int)((uint)(ushort)x | ((uint)(ushort)y << 16)))); }
    public static bool ActivateOwnedWindow(IntPtr window) {
        uint foregroundProcess;
        uint foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), out foregroundProcess);
        uint callingThread = GetCurrentThreadId();
        bool attached = foregroundThread != 0 && foregroundThread != callingThread && AttachThreadInput(callingThread, foregroundThread, true);
        try {
            BringWindowToTop(window);
            SetForegroundWindow(window);
            return GetForegroundWindow() == window;
        } finally {
            if (attached) AttachThreadInput(callingThread, foregroundThread, false);
        }
    }
}
'@ }
function Wait-CaptureCondition([scriptblock]$Condition, [string]$Failure) {
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 100
    }
    throw $Failure
}
function Find-CaptureDialog([int]$ProcessId) {
    [CaptureNative]::Find($ProcessId, '#32770', 'Save capture')
}
function Focus-CaptureOwner([IntPtr]$Window) {
    # Keyboard actions now belong to the foreground native surface. Show and
    # activate only the disposable test window, matching an actual user click.
    if (![CaptureNative]::IsWindowVisible($Window) -or [CaptureNative]::IsIconic($Window)) {
        [CaptureNative]::ShowWindow($Window, 9) | Out-Null
    }
    [CaptureNative]::ActivateOwnedWindow($Window) | Out-Null
    Wait-CaptureCondition { [CaptureNative]::GetForegroundWindow() -eq $Window } 'Validation window could not acquire keyboard focus.'
}
function Wait-CaptureDialog([int]$ProcessId) {
    Wait-CaptureCondition { (Find-CaptureDialog $ProcessId) -ne [IntPtr]::Zero } 'Save capture dialog did not appear.'
    Find-CaptureDialog $ProcessId
}
function Set-CaptureDialogPath([IntPtr]$Dialog, [string]$Path) {
    # The shell populates its filename edit asynchronously after creating the dialog.
    Wait-CaptureCondition { [CaptureNative]::FilenameEdit($Dialog) -ne [IntPtr]::Zero } 'Filename edit did not appear.'
    Start-Sleep -Milliseconds 500
    [CaptureNative]::SendMessage([CaptureNative]::FilenameEdit($Dialog), 0xC, [IntPtr]::Zero, $Path) | Out-Null
}
function Confirm-CaptureDialog([IntPtr]$Dialog) {
    $button = [CaptureNative]::Button($Dialog, 1)
    if ($button -eq [IntPtr]::Zero) { throw 'Save button not found.' }
    [CaptureNative]::PostMessage($button, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}
function Cancel-CaptureDialog([IntPtr]$Dialog) {
    [CaptureNative]::PostMessage($Dialog, 0x111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
    Wait-CaptureCondition { ![CaptureNative]::IsWindow($Dialog) } 'Capture dialog did not cancel.'
}
function Click-CaptureConfirmation([IntPtr]$Dialog, [int]$ButtonId) {
    $button = [CaptureNative]::Button($Dialog, $ButtonId)
    if ($button -ne [IntPtr]::Zero) {
        [CaptureNative]::PostMessage($button, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    } else {
        # Modern shell overwrite prompts are TaskDialog controls, not HWND Buttons.
        # TDM_CLICK_BUTTON is WM_USER + 102; IDYES/IDNO retain their standard IDs.
        [CaptureNative]::PostMessage($Dialog, 0x466, [IntPtr]$ButtonId, [IntPtr]::Zero) | Out-Null
    }
}
function Save-CaptureDialog([int]$ProcessId, [string]$Path, [string]$Preview = '') {
    $dialog = Wait-CaptureDialog $ProcessId
    Set-CaptureDialogPath $dialog $Path
    if ($Preview) {
        Add-Type -AssemblyName System.Drawing
        $rect = [CaptureNative+Rect]::new()
        [CaptureNative]::GetWindowRect($dialog, [ref]$rect) | Out-Null
        $bitmap = [Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $dc = $graphics.GetHdc()
        try { $printed = [CaptureNative]::PrintWindow($dialog, $dc, 2) }
        finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
        try { if ($printed) { $bitmap.Save($Preview, [Drawing.Imaging.ImageFormat]::Png) } }
        finally { $bitmap.Dispose() }
    }
    Confirm-CaptureDialog $dialog
    Wait-CaptureCondition { (Test-Path -LiteralPath $Path) -and ![CaptureNative]::IsWindow($dialog) } 'Capture was not saved at the selected path.'
}
function Save-CaptureOwnerFrame([IntPtr]$Window, [string]$Path) {
    # Read the composed desktop, not the DX11 back buffer: native overpainting
    # only appears on the actual HWND. The test keeps this title strip exposed.
    Add-Type -AssemblyName System.Drawing
    $rect = [CaptureNative+Rect]::new()
    [CaptureNative]::GetWindowRect($Window, [ref]$rect) | Out-Null
    $bitmap = [Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
        # Empty caption background, clear of logo/text/buttons in either theme.
        $pixel = $bitmap.GetPixel([int]($bitmap.Width * 0.6), 16)
        return $pixel
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Assert-CaptureOwnerFrame([IntPtr]$Window, [IntPtr]$Dialog, [string]$Path, $Expected) {
    if ([CaptureNative]::IsWindowEnabled($Window)) { throw 'Modal capture did not disable its owner.' }
    $rect = [CaptureNative+Rect]::new()
    [CaptureNative]::GetWindowRect($Window, [ref]$rect) | Out-Null
    # Keep the caption visible without reactivating the disabled owner.
    [CaptureNative]::SetWindowPos($Dialog, [IntPtr]::Zero, $rect.Left + 120, $rect.Top + 150, 0, 0, 0x15) | Out-Null
    Start-Sleep -Milliseconds 350
    $actual = Save-CaptureOwnerFrame $Window $Path
    $difference = [Math]::Abs($actual.R - $Expected.R) + [Math]::Abs($actual.G - $Expected.G) + [Math]::Abs($actual.B - $Expected.B)
    if ($difference -gt 12) { throw "Modal dialog repainted the custom caption: expected $Expected, actual $actual. Evidence: $Path" }
}
if ($HelpersOnly) { return }
[CaptureNative]::SetProcessDPIAware() | Out-Null

$root = Split-Path $PSScriptRoot -Parent
if (!$Executable) { $Executable = Join-Path $root 'build/gc-vs2022/Debug/RestArtEngine.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('RestArtEngine-CaptureTests/' + [guid]::NewGuid().ToString('N')) }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'Projects/Sandbox') -Destination $output -Recurse -Force
$manifest = Join-Path $output 'Sandbox/Sandbox.raproject'
$scene = Join-Path $output 'Sandbox/Content/Scenes/Default.rascene'
$sceneHash = (Get-FileHash -LiteralPath $scene).Hash
$chosenDirectory = Join-Path $output ([string][char]0x622A + [char]0x56FE + ' chosen folder')
New-Item -ItemType Directory -Force -Path $chosenDirectory | Out-Null
$ignoredDirectory = Join-Path $output 'cli-output'
$process = Start-Process -FilePath $Executable -ArgumentList @('--project', ('"' + $manifest + '"'), '--capture-dir', ('"' + $ignoredDirectory + '"')) -WorkingDirectory $output -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/stdout.txt" -RedirectStandardError "$output/stderr.txt"
$null = $process.Handle
$events = [System.Collections.Generic.List[string]]::new()
function Assert-Capture($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Capture-Key([int]$Key) {
    Focus-CaptureOwner $script:captureWindow
    [CaptureNative]::PostMessage($script:captureWindow, 0x100, [IntPtr]$Key, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 80
    [CaptureNative]::PostMessage($script:captureWindow, 0x101, [IntPtr]$Key, [IntPtr]::Zero) | Out-Null
}
try {
    Wait-CaptureCondition { [CaptureNative]::Find($process.Id, 'RAEngineMaterialLab', $null) -ne [IntPtr]::Zero } 'Editor window did not appear.'
    $script:captureWindow = [CaptureNative]::Find($process.Id, 'RAEngineMaterialLab', $null)
    [CaptureNative]::SetWindowPos($script:captureWindow, [IntPtr](-1), 80, 80, 1440, 900, 0x40) | Out-Null
    Start-Sleep -Seconds 2
    Focus-CaptureOwner $script:captureWindow
    Start-Sleep -Milliseconds 250
    $caption = Save-CaptureOwnerFrame $script:captureWindow (Join-Path $output 'owner-before.png')
    Capture-Key 0x78
    $dialog = Wait-CaptureDialog $process.Id
    Assert-CaptureOwnerFrame $script:captureWindow $dialog (Join-Path $output 'owner-modal-f9.png') $caption
    Cancel-CaptureDialog $dialog
    Start-Sleep -Milliseconds 250
    Assert-Capture (!(Test-Path -LiteralPath (Join-Path $output 'screenshots'))) 'Cancel created the legacy screenshots directory.'
    Assert-Capture (!(Test-Path -LiteralPath $ignoredDirectory)) 'Manual capture used --capture-dir instead of the dialog.'
    Assert-Capture ([CaptureNative]::IsWindowEnabled($script:captureWindow)) 'Cancel did not re-enable the owner.'
    $restoredCaption = Save-CaptureOwnerFrame $script:captureWindow (Join-Path $output 'owner-after.png')
    Assert-Capture ($restoredCaption.ToArgb() -eq $caption.ToArgb()) 'Cancel did not preserve the custom caption.'
    $events.Add('PASS: F9 opens Save capture; cancel writes nothing and ignores automated capture output.')

    $selected = Join-Path $chosenDirectory ([string][char]0x6D4B + [char]0x8BD5 + '.bmp')
    Capture-Key 0x78
    Save-CaptureDialog $process.Id $selected (Join-Path $output 'save-dialog.png')
    $image = [IO.File]::ReadAllBytes($selected)
    Assert-Capture ($image.Length -gt 54 -and $image[0] -eq 66 -and $image[1] -eq 77) 'Selected file is not a BMP.'
    Assert-Capture ([BitConverter]::ToInt32($image, 18) -gt 0 -and [BitConverter]::ToInt32($image, 22) -ne 0) 'BMP has invalid dimensions.'
    $events.Add('PASS: F9 saves a valid BMP at the selected Unicode path.')

    $replace = Join-Path $chosenDirectory 'replace.bmp'
    [IO.File]::WriteAllText($replace, 'preserve this existing user file until overwrite is confirmed')
    $originalHash = (Get-FileHash -LiteralPath $replace).Hash
    Capture-Key 0x78
    $dialog = Wait-CaptureDialog $process.Id
    Set-CaptureDialogPath $dialog $replace
    Confirm-CaptureDialog $dialog
    Wait-CaptureCondition { [CaptureNative]::FindConfirmation($process.Id, $dialog) -ne [IntPtr]::Zero } 'Overwrite confirmation did not appear.'
    $confirmation = [CaptureNative]::FindConfirmation($process.Id, $dialog)
    Assert-CaptureOwnerFrame $script:captureWindow $confirmation (Join-Path $output 'owner-modal-overwrite.png') $caption
    Click-CaptureConfirmation $confirmation 7
    Wait-CaptureCondition { ![CaptureNative]::IsWindow($confirmation) } 'Overwrite rejection did not close confirmation.'
    Assert-Capture ((Get-FileHash -LiteralPath $replace).Hash -eq $originalHash) 'Rejected overwrite changed the existing file.'
    Confirm-CaptureDialog $dialog
    Wait-CaptureCondition { [CaptureNative]::FindConfirmation($process.Id, $dialog) -ne [IntPtr]::Zero } 'Second overwrite confirmation did not appear.'
    $confirmation = [CaptureNative]::FindConfirmation($process.Id, $dialog)
    Click-CaptureConfirmation $confirmation 6
    Wait-CaptureCondition { ![CaptureNative]::IsWindow($dialog) } 'Confirmed overwrite did not finish.'
    $replacement = [IO.File]::ReadAllBytes($replace)
    Assert-Capture ($replacement.Length -gt 54 -and $replacement[0] -eq 66 -and $replacement[1] -eq 77) 'Confirmed overwrite did not write BMP data.'
    $events.Add('PASS: rejected overwrite preserves the old file; confirmed overwrite writes BMP data.')

    # Default 1440px editor toolbar: Capture button at x=403..471, y=80..104.
    $cursorBefore = [CaptureNative+Point]::new()
    [CaptureNative]::GetCursorPos([ref]$cursorBefore) | Out-Null
    $point = [CaptureNative+Point]::new(); $point.X = 435; $point.Y = 92
    [CaptureNative]::ClientToScreen($script:captureWindow, [ref]$point) | Out-Null
    [CaptureNative]::SetForegroundWindow($script:captureWindow) | Out-Null
    [CaptureNative]::SetCursorPos($point.X, $point.Y) | Out-Null
    Start-Sleep -Milliseconds 250
    [CaptureNative]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [CaptureNative]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    $dialog = Wait-CaptureDialog $process.Id
    Assert-CaptureOwnerFrame $script:captureWindow $dialog (Join-Path $output 'owner-modal-toolbar.png') $caption
    Cancel-CaptureDialog $dialog
    $events.Add('PASS: toolbar Capture uses the same cancellable save dialog.')
    $events.Add('PASS: custom caption pixels survive F9, toolbar Capture and nested overwrite dialogs; modal owner remains disabled and cancel re-enables it.')
    Assert-Capture ((Get-FileHash -LiteralPath $scene).Hash -eq $sceneHash) 'Capture changed the scene.'
    [CaptureNative]::PostMessage($script:captureWindow, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Assert-Capture ($process.WaitForExit(10000) -and $process.ExitCode -eq 0) 'Editor did not exit cleanly after capture.'
    Assert-Capture ((Get-Content "$output/stdout.txt" -Raw) -match 'validation errors: 0') 'D3D11 validation did not finish cleanly.'
    $events.Add('PASS: unchanged scene, clean exit, D3D11 validation errors 0.')
    $events | Set-Content -LiteralPath "$output/results.txt" -Encoding utf8
    $events
    "Evidence: $output"
} finally {
    if ($cursorBefore) { [CaptureNative]::SetCursorPos($cursorBefore.X, $cursorBefore.Y) | Out-Null }
    $events | Set-Content -LiteralPath "$output/results.txt" -Encoding utf8
    if (!$process.HasExited) { Stop-Process -Id $process.Id }
    $process.Dispose()
}
