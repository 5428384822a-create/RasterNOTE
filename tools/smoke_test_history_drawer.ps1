param(
    [string]$ExePath = ".\build\Release\RasterNoteNative.exe",
    [string]$OutputDirectory = ".\artifacts\smoke"
)

Add-Type -AssemblyName System.Drawing

$source = @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;

public static class WindowHarness {
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    private static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    private static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT {
        public int X;
        public int Y;
    }

    public static IntPtr FindWindowByTitle(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hWnd, lParam) => {
            if (!IsWindowVisible(hWnd)) {
                return true;
            }

            var builder = new StringBuilder(512);
            GetWindowTextW(hWnd, builder, builder.Capacity);
            var current = builder.ToString();
            if (!string.IsNullOrWhiteSpace(current) &&
                current.IndexOf(title, StringComparison.OrdinalIgnoreCase) >= 0) {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static RECT GetRect(IntPtr hwnd) {
        RECT rect;
        GetWindowRect(hwnd, out rect);
        return rect;
    }

    public static RECT GetClientRectPx(IntPtr hwnd) {
        RECT rect;
        GetClientRect(hwnd, out rect);
        return rect;
    }

    public static double GetScale(IntPtr hwnd) {
        return Math.Max(1.0, GetDpiForWindow(hwnd) / 96.0);
    }

    public static void Activate(IntPtr hwnd) {
        const uint SWP_NOSIZE = 0x0001;
        const uint SWP_NOMOVE = 0x0002;
        const uint SWP_SHOWWINDOW = 0x0040;
        SetWindowPos(hwnd, new IntPtr(-1), 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
    }

    private static POINT DipToScreenPoint(IntPtr hwnd, double dipX, double dipY) {
        var scale = GetScale(hwnd);
        var point = new POINT {
            X = (int)Math.Round(dipX * scale),
            Y = (int)Math.Round(dipY * scale)
        };
        ClientToScreen(hwnd, ref point);
        return point;
    }

    public static void ClickDip(IntPtr hwnd, float dipX, float dipY) {
        var scale = GetScale(hwnd);
        var x = (int)Math.Round(dipX * scale);
        var y = (int)Math.Round(dipY * scale);
        var lParam = (IntPtr)((y << 16) | (x & 0xFFFF));
        SendMessageW(hwnd, 0x0200, IntPtr.Zero, lParam);
        SendMessageW(hwnd, 0x0201, (IntPtr)0x0001, lParam);
        SendMessageW(hwnd, 0x0202, IntPtr.Zero, lParam);
    }

    public static RECT DragDip(IntPtr hwnd, float startDipX, float startDipY, float deltaDipX, float deltaDipY) {
        var start = DipToScreenPoint(hwnd, startDipX, startDipY);
        var end = DipToScreenPoint(hwnd, startDipX + deltaDipX, startDipY + deltaDipY);

        SetCursorPos(start.X, start.Y);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
        for (int i = 1; i <= 8; ++i) {
            var x = start.X + (end.X - start.X) * i / 8;
            var y = start.Y + (end.Y - start.Y) * i / 8;
            SetCursorPos(x, y);
            System.Threading.Thread.Sleep(18);
        }
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(120);
        return GetRect(hwnd);
    }

    private static void SendCtrlShortcut(byte virtualKey) {
        const byte VK_CONTROL = 0x11;
        const uint KEYEVENTF_KEYUP = 0x0002;
        keybd_event(VK_CONTROL, 0, 0, UIntPtr.Zero);
        keybd_event(virtualKey, 0, 0, UIntPtr.Zero);
        keybd_event(virtualKey, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    public static void SendCtrlN() {
        SendCtrlShortcut(0x4E);
    }

    public static void SendCtrlH() {
        SendCtrlShortcut(0x48);
    }
}
"@

Add-Type -TypeDefinition $source

$Theme = @{
    OuterPadding = 18.0
    HeaderHeight = 92.0
    ButtonWidth = 56.0
    ButtonHeight = 24.0
    WindowControlWidth = 56.0
    ControlGap = 6.0
    HistoryDrawerWidth = 336.0
    HistoryDrawerHeaderHeight = 48.0
    HistoryRowHeight = 74.0
    HistoryRowGap = 8.0
    HistoryDeleteButtonWidth = 64.0
}

function Get-MainLayoutDip {
    param([IntPtr]$Hwnd)

    $clientRect = [WindowHarness]::GetClientRectPx($Hwnd)
    $scale = [WindowHarness]::GetScale($Hwnd)
    $clientWidthDip = ($clientRect.Right - $clientRect.Left) / $scale
    $innerRight = ($clientWidthDip - $Theme.OuterPadding) - 16.0
    $chromeBottom = $Theme.OuterPadding + $Theme.HeaderHeight
    $editorRight = $clientWidthDip - $Theme.OuterPadding
    $drawerLeft = $editorRight - $Theme.HistoryDrawerWidth
    $drawerTop = $chromeBottom + 1.0
    $drawerHeaderBottom = ($drawerTop + 1.0) + $Theme.HistoryDrawerHeaderHeight
    $listTop = $drawerHeaderBottom + 10.0
    $listRight = $editorRight - 12.0
    $listLeft = $drawerLeft + 12.0

    return [pscustomobject]@{
        ListLeft = $listLeft
        ListRight = $listRight
        ListTop = $listTop
    }
}

function Get-HistoryRowCenterDip {
    param(
        [object]$Layout,
        [int]$Index
    )

    $rowTop = $Layout.ListTop + $Index * ($Theme.HistoryRowHeight + $Theme.HistoryRowGap)
    return [pscustomobject]@{
        X = $Layout.ListLeft + 56.0
        Y = $rowTop + ($Theme.HistoryRowHeight / 2.0)
    }
}

function Get-HistoryDeleteCenterDip {
    param(
        [object]$Layout,
        [int]$Index
    )

    $rowTop = $Layout.ListTop + $Index * ($Theme.HistoryRowHeight + $Theme.HistoryRowGap)
    return [pscustomobject]@{
        X = $Layout.ListRight - 10.0 - ($Theme.HistoryDeleteButtonWidth / 2.0)
        Y = $rowTop + 20.0
    }
}

function Get-HistoryDeleteCandidatesDip {
    param(
        [object]$Layout,
        [int]$Index
    )

    $rowTop = $Layout.ListTop + $Index * ($Theme.HistoryRowHeight + $Theme.HistoryRowGap)
    $rowRight = $Layout.ListRight
    return @(
        [pscustomobject]@{ X = $rowRight - 42.0; Y = $rowTop + 20.0 }
        [pscustomobject]@{ X = $rowRight - 54.0; Y = $rowTop + 20.0 }
        [pscustomobject]@{ X = $rowRight - 30.0; Y = $rowTop + 20.0 }
        [pscustomobject]@{ X = $rowRight - 42.0; Y = $rowTop + 24.0 }
    )
}

function Get-HistoryConfirmCandidatesDip {
    param(
        [object]$Layout,
        [int]$Index
    )

    $rowTop = $Layout.ListTop + $Index * ($Theme.HistoryRowHeight + $Theme.HistoryRowGap)
    $rowRight = $Layout.ListRight
    return @(
        [pscustomobject]@{ X = $rowRight - 106.0; Y = $rowTop + 49.0 }
        [pscustomobject]@{ X = $rowRight - 118.0; Y = $rowTop + 49.0 }
        [pscustomobject]@{ X = $rowRight - 94.0; Y = $rowTop + 49.0 }
        [pscustomobject]@{ X = $rowRight - 106.0; Y = $rowTop + 45.0 }
    )
}

function Get-NoteCount {
    $notesPath = Join-Path $env:LOCALAPPDATA "RasterNoteNative\\notes"
    if (-not (Test-Path $notesPath)) {
        return 0
    }
    return (Get-ChildItem -Path $notesPath -Filter "*.rnote" -ErrorAction SilentlyContinue).Count
}

$resolvedExe = [System.IO.Path]::GetFullPath($ExePath)
$resolvedOutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($resolvedOutputDirectory) | Out-Null
$captureScript = [System.IO.Path]::GetFullPath(".\tools\capture_window.ps1")

$initialCount = Get-NoteCount
$process = Start-Process -FilePath $resolvedExe -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $hwnd = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline) {
        $hwnd = [WindowHarness]::FindWindowByTitle("RasterNoteNative")
        if ($hwnd -ne [IntPtr]::Zero) {
            break
        }
        Start-Sleep -Milliseconds 150
    }

    if ($hwnd -eq [IntPtr]::Zero) {
        throw "RasterNoteNative window not found."
    }

    [WindowHarness]::Activate($hwnd)
    Start-Sleep -Milliseconds 250
    $layout = Get-MainLayoutDip -Hwnd $hwnd
    $startRect = [WindowHarness]::GetRect($hwnd)
    $endRect = [WindowHarness]::DragDip($hwnd, 160, 28, 90, 50)
    $dragWorked = ($startRect.Left -ne $endRect.Left) -or ($startRect.Top -ne $endRect.Top)

    [WindowHarness]::Activate($hwnd)
    Start-Sleep -Milliseconds 120
    [WindowHarness]::SendCtrlH()
    Start-Sleep -Milliseconds 320
    & $captureScript -WindowTitle "RasterNoteNative" -OutputPath (Join-Path $resolvedOutputDirectory "drawer-open.png") -WaitMilliseconds 5000 | Out-Null

    $openRowIndex = if ($initialCount -gt 0) { 1 } else { 0 }
    $openRowPoint = Get-HistoryRowCenterDip -Layout $layout -Index $openRowIndex
    [WindowHarness]::ClickDip($hwnd, $openRowPoint.X, $openRowPoint.Y)
    Start-Sleep -Milliseconds 350
    & $captureScript -WindowTitle "RasterNoteNative" -OutputPath (Join-Path $resolvedOutputDirectory "opened-old-note.png") -WaitMilliseconds 5000 | Out-Null

    [WindowHarness]::Activate($hwnd)
    Start-Sleep -Milliseconds 120
    [WindowHarness]::SendCtrlN()
    Start-Sleep -Milliseconds 350
    [WindowHarness]::Activate($hwnd)
    Start-Sleep -Milliseconds 120
    [WindowHarness]::SendCtrlH()
    Start-Sleep -Milliseconds 320
    $beforeDeleteCount = Get-NoteCount

    $afterDeleteCount = $beforeDeleteCount
    $capturedDeleteState = $false
    foreach ($deletePoint in (Get-HistoryDeleteCandidatesDip -Layout $layout -Index 0)) {
        [WindowHarness]::ClickDip($hwnd, $deletePoint.X, $deletePoint.Y)
        Start-Sleep -Milliseconds 180
        if (-not $capturedDeleteState) {
            & $captureScript -WindowTitle "RasterNoteNative" -OutputPath (Join-Path $resolvedOutputDirectory "delete-confirm.png") -WaitMilliseconds 5000 | Out-Null
            $capturedDeleteState = $true
        }

        foreach ($confirmPoint in (Get-HistoryConfirmCandidatesDip -Layout $layout -Index 0)) {
            [WindowHarness]::ClickDip($hwnd, $confirmPoint.X, $confirmPoint.Y)
            Start-Sleep -Milliseconds 280
            $afterDeleteCount = Get-NoteCount
            if ($afterDeleteCount -lt $beforeDeleteCount) {
                break 2
            }
        }
    }

    & $captureScript -WindowTitle "RasterNoteNative" -OutputPath (Join-Path $resolvedOutputDirectory "after-delete-current-temp.png") -WaitMilliseconds 5000 | Out-Null

    [pscustomobject]@{
        DragWorked = $dragWorked
        InitialCount = $initialCount
        BeforeDeleteCount = $beforeDeleteCount
        AfterDeleteCount = $afterDeleteCount
        SmokeDirectory = $resolvedOutputDirectory
    } | Format-List
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id
    }
}
