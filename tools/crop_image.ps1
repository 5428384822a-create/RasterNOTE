param(
    [string]$InputPath,
    [string]$OutputPath,
    [int]$X,
    [int]$Y,
    [int]$Width,
    [int]$Height
)

Add-Type -AssemblyName System.Drawing

$source = [System.Drawing.Bitmap]::FromFile([System.IO.Path]::GetFullPath($InputPath))
try {
    $crop = New-Object System.Drawing.Bitmap $Width, $Height
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($crop)
        try {
            $graphics.DrawImage(
                $source,
                [System.Drawing.Rectangle]::new(0, 0, $Width, $Height),
                [System.Drawing.Rectangle]::new($X, $Y, $Width, $Height),
                [System.Drawing.GraphicsUnit]::Pixel
            )

            $resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
            $directory = [System.IO.Path]::GetDirectoryName($resolvedOutput)
            if (-not [string]::IsNullOrWhiteSpace($directory)) {
                [System.IO.Directory]::CreateDirectory($directory) | Out-Null
            }

            $crop.Save($resolvedOutput, [System.Drawing.Imaging.ImageFormat]::Png)
            Write-Output $resolvedOutput
        } finally {
            $graphics.Dispose()
        }
    } finally {
        $crop.Dispose()
    }
} finally {
    $source.Dispose()
}
