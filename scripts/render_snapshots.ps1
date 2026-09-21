<#
.SYNOPSIS
    Renders visual assets and snapshots for Stuttometer documentation into assets/.

.DESCRIPTION
    Generates high-resolution PNG assets with the unified 4-attribution palette:
    - GAME ENGINE: #daa142
    - DWM COMPOSITION: #c55656
    - EXTERNAL CONTENTION: #9a64cd
    - UNKNOWN: #697382
    Along with refresh-rate-aware severity colors (#f1f5f9, #f59e0b, #ef4444).

.EXAMPLE
    .\scripts\render_snapshots.ps1
#>
[CmdletBinding()]
param(
    [string]$AssetsDir
)

$ErrorActionPreference = "Stop"

if (-not $AssetsDir) {
    $AssetsDir = Join-Path $PSScriptRoot "..\assets"
}
$assetsPath = [System.IO.Path]::GetFullPath($AssetsDir)

if (-not (Test-Path $assetsPath)) {
    New-Item -ItemType Directory -Path $assetsPath -Force | Out-Null
}

Add-Type -AssemblyName System.Drawing

function Render-OsdToast {
    param(
        [string]$OutputPath,
        [float]$Scale = 2.0,
        [string]$ProcessName = "Cyberpunk2077.exe",
        [string]$Callout = "68.4 ms STUTTER",
        [string]$CalloutHex = "#ef4444",
        [string]$DiagSummary = "nvlddmkm.sys: DPC routine execution spike (94% Conf)",
        [string]$AttrTag = "EXTERNAL CONTENTION",
        [string]$AccentHex = "#9a64cd",
        [string]$ConfText = "",
        [int]$Alpha = 235,
        [bool]$AddShadow = $true
    )

    $w = [int](360 * $Scale)
    $h = [int](80 * $Scale)
    $margin = if ($AddShadow) { [int](12 * $Scale) } else { 0 }
    
    $totalW = $w + ($margin * 2)
    $totalH = $h + ($margin * 2)

    $bmp = New-Object System.Drawing.Bitmap($totalW, $totalH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor

    # Clear transparent
    $g.Clear([System.Drawing.Color]::Transparent)

    $toastX = $margin
    $toastY = $margin

    # Draw subtle drop shadow if requested
    if ($AddShadow) {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        for ($i = 8; $i -ge 1; $i--) {
            $shadowAlpha = [int](22 / $i)
            $sBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($shadowAlpha, 0, 0, 0))
            $sRect = New-Object System.Drawing.RectangleF(
                ($toastX - $i * $Scale * 0.4), 
                ($toastY + $i * $Scale * 0.4), 
                ($w + $i * $Scale * 0.8), 
                ($h + $i * $Scale * 0.8)
            )
            $g.FillRectangle($sBrush, $sRect)
            $sBrush.Dispose()
        }
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    }

    # Background (#11151f)
    $bgBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($Alpha, 17, 21, 31))
    $toastRect = New-Object System.Drawing.Rectangle($toastX, $toastY, $w, $h)
    $g.FillRectangle($bgBrush, $toastRect)

    # 1px border (#2a354b)
    $penColor = [System.Drawing.Color]::FromArgb($Alpha, 42, 53, 75)
    $pen = New-Object System.Drawing.Pen($penColor, [int](1.0 * $Scale))
    $pen.Alignment = [System.Drawing.Drawing2D.PenAlignment]::Inset
    $g.DrawRectangle($pen, $toastX, $toastY, ($w - 1), ($h - 1))

    # Left accent stripe (5px wide)
    $accentColor = [System.Drawing.ColorTranslator]::FromHtml($AccentHex)
    $stripeBrush = New-Object System.Drawing.SolidBrush($accentColor)
    $stripeW = [int](5 * $Scale)
    $g.FillRectangle($stripeBrush, $toastX, $toastY, $stripeW, $h)

    # Fonts (matching pixel sizes from osd_toast.cpp: 13px, 11px, 10px)
    $fontTitle = New-Object System.Drawing.Font("Segoe UI", (13 * $Scale), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fontMain  = New-Object System.Drawing.Font("Segoe UI", (11 * $Scale), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $fontSub   = New-Object System.Drawing.Font("Segoe UI", (10 * $Scale), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)

    $textPriBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(241, 245, 249))
    $textSecBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(148, 163, 184))
    $calloutColor = [System.Drawing.ColorTranslator]::FromHtml($CalloutHex)
    $calloutBrush = New-Object System.Drawing.SolidBrush($calloutColor)
    $attrBrush    = New-Object System.Drawing.SolidBrush($accentColor)

    $padLeft  = [int](16 * $Scale)
    $padRight = [int](14 * $Scale)

    # String formats
    $formatLeft = New-Object System.Drawing.StringFormat
    $formatLeft.Alignment = [System.Drawing.StringAlignment]::Near
    $formatLeft.LineAlignment = [System.Drawing.StringAlignment]::Center
    $formatLeft.Trimming = [System.Drawing.StringTrimming]::EllipsisCharacter
    $formatLeft.FormatFlags = [System.Drawing.StringFormatFlags]::NoWrap

    $formatRight = New-Object System.Drawing.StringFormat
    $formatRight.Alignment = [System.Drawing.StringAlignment]::Far
    $formatRight.LineAlignment = [System.Drawing.StringAlignment]::Center
    $formatRight.FormatFlags = [System.Drawing.StringFormatFlags]::NoWrap

    # Row 1: Process Name + Severity Callout
    $r1Y = $toastY + [int](8 * $Scale)
    $r1H = [int](20 * $Scale)
    $calloutW = [int](156 * $Scale)

    $rcCallout = New-Object System.Drawing.RectangleF(($toastX + $w - $padRight - $calloutW), $r1Y, $calloutW, $r1H)
    $g.DrawString($Callout, $fontTitle, $calloutBrush, $rcCallout, $formatRight)

    $rcProc = New-Object System.Drawing.RectangleF(($toastX + $padLeft), $r1Y, ($w - $padLeft - $padRight - $calloutW - [int](8 * $Scale)), $r1H)
    $g.DrawString($ProcessName, $fontTitle, $textPriBrush, $rcProc, $formatLeft)

    # Row 2: Diagnostic summary
    $r2Y = $toastY + [int](30 * $Scale)
    $r2H = [int](22 * $Scale)
    $rcDiag = New-Object System.Drawing.RectangleF(($toastX + $padLeft), $r2Y, ($w - $padLeft - $padRight), $r2H)
    $g.DrawString($DiagSummary, $fontMain, $textSecBrush, $rcDiag, $formatLeft)

    # Row 3: Attribution Tag (Left) + Confidence (Right)
    $r3Y = $toastY + [int](54 * $Scale)
    $r3H = [int](20 * $Scale)
    $confW = [int](80 * $Scale)
    $rcTag = New-Object System.Drawing.RectangleF(($toastX + $padLeft), $r3Y, ($w - $padLeft - $padRight - $confW), $r3H)
    $g.DrawString($AttrTag, $fontSub, $attrBrush, $rcTag, $formatLeft)

    if ($ConfText) {
        $rcConf = New-Object System.Drawing.RectangleF(($toastX + $w - $padRight - $confW), $r3Y, $confW, $r3H)
        $g.DrawString($ConfText, $fontSub, $textSecBrush, $rcConf, $formatRight)
    }

    # Cleanup
    $fontTitle.Dispose()
    $fontMain.Dispose()
    $fontSub.Dispose()
    $textPriBrush.Dispose()
    $textSecBrush.Dispose()
    $calloutBrush.Dispose()
    $attrBrush.Dispose()
    $stripeBrush.Dispose()
    $bgBrush.Dispose()
    $pen.Dispose()
    $g.Dispose()

    $bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function Render-InGameContext {
    param(
        [string]$OutputPath,
        [string]$ToastImagePath
    )

    $screenW = 1280
    $screenH = 720
    $bmp = New-Object System.Drawing.Bitmap($screenW, $screenH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    # Dark atmospheric game viewport backdrop
    $rect = New-Object System.Drawing.Rectangle(0, 0, $screenW, $screenH)
    $linBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 14, 18, 28),
        [System.Drawing.Color]::FromArgb(255, 6, 8, 14),
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal
    )
    $g.FillRectangle($linBrush, $rect)
    $linBrush.Dispose()

    # Subtle perspective grid
    $gridPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(22, 59, 130, 246), 1)
    for ($x = 0; $x -le $screenW; $x += 64) {
        $g.DrawLine($gridPen, $x, 0, $x, $screenH)
    }
    for ($y = 0; $y -le $screenH; $y += 48) {
        $g.DrawLine($gridPen, 0, $y, $screenW, $y)
    }
    $gridPen.Dispose()

    # Center crosshair
    $crossPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(90, 148, 163, 184), 1.5)
    $cx = [int]($screenW / 2)
    $cy = [int]($screenH / 2)
    $g.DrawLine($crossPen, ($cx - 14), $cy, ($cx - 4), $cy)
    $g.DrawLine($crossPen, ($cx + 4), $cy, ($cx + 14), $cy)
    $g.DrawLine($crossPen, $cx, ($cy - 14), $cx, ($cy - 4))
    $g.DrawLine($crossPen, $cx, ($cy + 4), $cx, ($cy + 14))
    $crossPen.Dispose()

    # Bottom-left game HUD status
    $hudTitleFont = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $hudSubFont = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Regular)
    $hudPriBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(120, 241, 245, 249))
    $hudSecBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(80, 148, 163, 184))
    $bullet = [char]0x2022
    $g.DrawString("Cyberpunk 2077 [DX12 Direct3D 12]", $hudTitleFont, $hudPriBrush, 28, ($screenH - 58))
    $g.DrawString("1440p High $bullet Ray Tracing Ultra $bullet DLSS Quality", $hudSubFont, $hudSecBrush, 28, ($screenH - 36))
    $hudTitleFont.Dispose()
    $hudSubFont.Dispose()
    $hudPriBrush.Dispose()
    $hudSecBrush.Dispose()

    # Draw OSD Toast in Top-Right corner (24px pad from edge)
    $toastImg = [System.Drawing.Image]::FromFile($ToastImagePath)
    $destW = [int]($toastImg.Width / 2)
    $destH = [int]($toastImg.Height / 2)
    $destX = $screenW - 24 - ($destW - 12)
    $destY = 24 - 12
    
    $g.DrawImage($toastImg, $destX, $destY, $destW, $destH)
    $toastImg.Dispose()

    # Annotate Top-Right
    $annFont = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
    $annBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(160, 148, 163, 184))
    $g.DrawString("TOP-RIGHT IN-GAME NOTIFICATION (NON-INTRUSIVE GDI OVERLAY)", $annFont, $annBrush, ($screenW - 460), (24 + 80 + 14))
    $annFont.Dispose()
    $annBrush.Dispose()

    $g.Dispose()
    $bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

Write-Host "Rendering OSD Toast assets into $assetsPath..." -ForegroundColor Cyan

# 1. Primary OSD Toast with Elevation Shadow (External Contention - Soft Purple #9a64cd, Danger #ef4444)
Render-OsdToast -OutputPath "$assetsPath\osd_toast.png" -Scale 2.0 `
    -ProcessName "Cyberpunk2077.exe" `
    -Callout "68.4 ms STUTTER" -CalloutHex "#ef4444" `
    -DiagSummary "nvlddmkm.sys: DPC routine execution spike" `
    -AttrTag "EXTERNAL CONTENTION" -AccentHex "#9a64cd" `
    -ConfText "94% CONF" -AddShadow $true

# 2. Frame-Bounded Clean PNG (No outer margin/shadow)
Render-OsdToast -OutputPath "$assetsPath\osd_toast_clean.png" -Scale 2.0 `
    -ProcessName "Cyberpunk2077.exe" `
    -Callout "68.4 ms STUTTER" -CalloutHex "#ef4444" `
    -DiagSummary "nvlddmkm.sys: DPC routine execution spike" `
    -AttrTag "EXTERNAL CONTENTION" -AccentHex "#9a64cd" `
    -ConfText "94% CONF" -AddShadow $false

# 3. Game Engine Stall Snapshot (Game Engine - Warm Gold #daa142, Warning #f59e0b)
Render-OsdToast -OutputPath "$assetsPath\osd_toast_game_engine.png" -Scale 2.0 `
    -ProcessName "EldenRing.exe" `
    -Callout "34.2 ms STUTTER" -CalloutHex "#f59e0b" `
    -DiagSummary "Main thread stalled waiting on worker thread lock" `
    -AttrTag "GAME ENGINE" -AccentHex "#daa142" `
    -ConfText "88% CONF" -AddShadow $true

# 4. Audio Glitch Snapshot (External Contention - Soft Purple #9a64cd, Danger #ef4444)
Render-OsdToast -OutputPath "$assetsPath\osd_toast_audio_glitch.png" -Scale 2.0 `
    -ProcessName "audiodg.exe" `
    -Callout "AUDIO GLITCH (x2)" -CalloutHex "#ef4444" `
    -DiagSummary "RealtekAudio.sys: Endpoint buffer underrun detected" `
    -AttrTag "EXTERNAL CONTENTION" -AccentHex "#9a64cd" `
    -ConfText "96% CONF" -AddShadow $true

# 5. In-Game Context View
Render-InGameContext -OutputPath "$assetsPath\osd_toast_ingame.png" -ToastImagePath "$assetsPath\osd_toast.png"

# 6. Render C++ Visual Card via test_card_renderer with STUTTO_DUMP_CARD_DIR
$cardTestExe = "$PSScriptRoot\..\build\Release\test_card_renderer.exe"
if (Test-Path $cardTestExe) {
    Write-Host "Rendering Visual Card via CardRenderer engine..." -ForegroundColor Cyan
    $env:STUTTO_DUMP_CARD_DIR = $assetsPath
    & $cardTestExe | Out-Null
    Remove-Item Env:\STUTTO_DUMP_CARD_DIR
    # Clean up unreferenced auxiliary cards to keep repo clean
    Remove-Item -Path "$assetsPath\dummy_card_audio_glitch.png", "$assetsPath\dummy_card_contention.png", "$assetsPath\dummy_card_dwm.png" -ErrorAction SilentlyContinue
}

Write-Host "Visual assets rendered successfully to $assetsPath!" -ForegroundColor Green
