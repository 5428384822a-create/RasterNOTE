param(
    [string]$WindowTitle = "RasterNoteNative",
    [string]$OutputPath = ".\artifacts\window-capture.png",
    [int]$WaitMilliseconds = 8000
)

Add-Type -AssemblyName System.Drawing

$source = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WindowCaptureNative {
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
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
        if (hwnd == IntPtr.Zero) {
            throw new InvalidOperationException("Target window was not found.");
        }

        RECT rect;
        if (!GetWindowRect(hwnd, out rect)) {
            throw new InvalidOperationException("Failed to read window bounds.");
        }
        return rect;
    }

}
"@

Add-Type -TypeDefinition $source

$deadline = [DateTime]::UtcNow.AddMilliseconds($WaitMilliseconds)
$target = [IntPtr]::Zero
while ([DateTime]::UtcNow -lt $deadline) {
    $target = [WindowCaptureNative]::FindWindowByTitle($WindowTitle)
    if ($target -ne [IntPtr]::Zero) {
        break
    }
    Start-Sleep -Milliseconds 200
}

if ($target -eq [IntPtr]::Zero) {
    throw "Window '$WindowTitle' was not found."
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$directory = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if (-not [string]::IsNullOrWhiteSpace($directory)) {
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
}

$rect = [WindowCaptureNative]::GetRect($target)
$width = [Math]::Max(1, $rect.Right - $rect.Left)
$height = [Math]::Max(1, $rect.Bottom - $rect.Top)
$bitmap = [System.Drawing.Bitmap]::new($width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, [System.Drawing.Size]::new($width, $height))
        $bitmap.Save($resolvedOutput, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
    }
} finally {
    $bitmap.Dispose()
}

Write-Output $resolvedOutput
