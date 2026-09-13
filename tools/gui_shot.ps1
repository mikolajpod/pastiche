<#
.SYNOPSIS
  Drives pastiche-gui.exe from a script and captures window screenshots.

.DESCRIPTION
  There are no automated GUI tests (DECYZJE.md D12), but the window still has
  to be exercised after changes. This helper starts the GUI, replays a list of
  steps against it and saves PNGs of the window, so a change can be checked
  without a human at the keyboard.

  It drives the real mouse and keyboard, so it takes over the pointer for the
  duration of the run. Coordinates are client-area pixels of the GUI window
  (0,0 = just under the title bar), which is what the saved screenshots show
  minus an 8 px left border and a 31 px top border.

.PARAMETER Steps
  Ordered list of actions:
    click:X,Y      left click at client position
    dclick:X,Y     double click (selects the text of an input field)
    type:TEXT      send keystrokes ({ENTER}, ^a etc. as in SendKeys)
    wait:MS        sleep
    shot:FILE      save a PNG of the window

.EXAMPLE
  # Dot-source style invocation - "powershell -File" would pass -Steps as a
  # single string instead of an array.
  & .\tools\gui_shot.ps1 -Exe build\pastiche-gui.exe `
      -AppArgs D:\hg\style\testdata\content.png,D:\hg\style\testdata\style.png `
      -Steps "shot:build/gui_idle.png","click:771,754","wait:4000","shot:build/gui_done.png"

.NOTES
  Client coordinates of the controls in the default 1400x900 window:
    Run 771,754   Cancel 899,754   size field 912,638   backend combo 935,684
    (combo items open below it: auto 935,707  dml 935,725  cpu 725,742)
  They shift by one line height (about 17 px) when the GPU estimate or the
  refusal message appears, so take a screenshot first when in doubt.
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string[]]$AppArgs = @(),
    [Parameter(Mandatory = $true)][string[]]$Steps,
    [int]$StartupMs = 4000
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PasticheGui {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

$proc = if ($AppArgs.Count -gt 0) {
    Start-Process -FilePath $Exe -ArgumentList $AppArgs -PassThru
} else {
    Start-Process -FilePath $Exe -PassThru
}
Start-Sleep -Milliseconds $StartupMs
$proc.Refresh()
if ($proc.HasExited) { Write-Output "FAIL: process exited early with code $($proc.ExitCode)"; exit 1 }
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "FAIL: no main window"; Stop-Process -Id $proc.Id -Force; exit 1 }
[void][PasticheGui]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 800

$origin = New-Object PasticheGui+POINT
[void][PasticheGui]::ClientToScreen($hwnd, [ref]$origin)
Write-Output "window '$($proc.MainWindowTitle)', client origin ($($origin.X),$($origin.Y))"

function Invoke-Click([int]$x, [int]$y, [int]$times) {
    [void][PasticheGui]::SetCursorPos($origin.X + $x, $origin.Y + $y)
    Start-Sleep -Milliseconds 250
    for ($i = 0; $i -lt $times; $i++) {
        [PasticheGui]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # left button down
        Start-Sleep -Milliseconds 60
        [PasticheGui]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # left button up
        Start-Sleep -Milliseconds 60
    }
}

function Save-Shot([string]$file) {
    $r = New-Object PasticheGui+RECT
    [void][PasticheGui]::GetWindowRect($hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap(($r.Right - $r.Left), ($r.Bottom - $r.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $dir = Split-Path -Parent $file
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "  shot -> $file"
}

$failed = $false
foreach ($step in $Steps) {
    $kind, $arg = $step.Split(':', 2)
    switch ($kind) {
        'click'  { $xy = $arg.Split(','); Invoke-Click ([int]$xy[0]) ([int]$xy[1]) 1; Write-Output "  click $arg" }
        'dclick' { $xy = $arg.Split(','); Invoke-Click ([int]$xy[0]) ([int]$xy[1]) 2; Write-Output "  dclick $arg" }
        'type'   { [System.Windows.Forms.SendKeys]::SendWait($arg); Write-Output "  type $arg" }
        'wait'   { Start-Sleep -Milliseconds ([int]$arg) }
        'shot'   { Save-Shot $arg }
        default  { Write-Output "FAIL: unknown step '$step'"; $failed = $true }
    }
    if ($failed) { break }
    $proc.Refresh()
    if ($proc.HasExited) { Write-Output "FAIL: process exited with code $($proc.ExitCode) during '$step'"; exit 1 }
}

Stop-Process -Id $proc.Id -Force
if ($failed) { exit 1 }
Write-Output "done"
