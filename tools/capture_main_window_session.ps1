param(
    [string]$ExePath = ".\build\Release\RasterNoteNative.exe",
    [string]$WindowTitle = "RasterNoteNative",
    [string]$OutputPath = ".\artifacts\main-window-capture.png"
)

$resolvedExe = [System.IO.Path]::GetFullPath($ExePath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$captureScript = [System.IO.Path]::GetFullPath(".\tools\capture_window.ps1")

$process = Start-Process -FilePath $resolvedExe -PassThru
try {
    Start-Sleep -Milliseconds 1000
    & $captureScript -WindowTitle $WindowTitle -OutputPath $resolvedOutput -WaitMilliseconds 8000
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id
    }
}
