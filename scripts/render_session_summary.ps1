[CmdletBinding()]
param(
    [string]$OutputPath,
    [float]$Scale = 1.5,
    [switch]$Populated
)

Add-Type -AssemblyName System.Drawing

if (-not $OutputPath) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $assetsDir = Join-Path $scriptDir "..\assets"
    if ($Populated) {
        $OutputPath = Join-Path $assetsDir "session_summary_populated.png"
    } else {
        $OutputPath = Join-Path $assetsDir "session_summary.png"
    }
}

function S([int]$v) { return [int]($v * $Scale) }

$emdash = [char]0x2014
$gte = [char]0x2265

$baseW = 780
$baseH = 640
$w = [int]($baseW * $Scale)
$h = [int]($baseH * $Scale)

$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

function Draw-RoundedCard($graphics, [System.Drawing.Rectangle]$rect, [System.Drawing.Color]$bg, [System.Drawing.Color]$border, [int]$radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $r = $rect
    $path.AddArc($r.X, $r.Y, $d, $d, 180, 90)
    $path.AddArc($r.Right - $d, $r.Y, $d, $d, 270, 90)
    $path.AddArc($r.Right - $d, $r.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($r.X, $r.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $bgBrush = New-Object System.Drawing.SolidBrush($bg)
    $graphics.FillPath($bgBrush, $path)
    $bgBrush.Dispose()

    $borderPen = New-Object System.Drawing.Pen($border, 1.0)
    $graphics.DrawPath($borderPen, $path)
    $borderPen.Dispose()
    $path.Dispose()
}

# 1. Canvas Background (#111317)
$bgCanvas = [System.Drawing.Color]::FromArgb(17, 19, 23)
$cardBg   = [System.Drawing.Color]::FromArgb(28, 33, 44)
$cardBorder = [System.Drawing.Color]::FromArgb(40, 48, 66)
$g.Clear($bgCanvas)

$margin = (S 16)
$fontTitle = New-Object System.Drawing.Font("Segoe UI", [float](S 13), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$fontMetric = New-Object System.Drawing.Font("Segoe UI", [float](S 16), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$fontBold = New-Object System.Drawing.Font("Segoe UI", [float](S 9), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$fontRegular = New-Object System.Drawing.Font("Segoe UI", [float](S 9), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$fontSmall = New-Object System.Drawing.Font("Segoe UI", [float](S 8), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

$brPri   = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(241, 245, 249))
$brLabel = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(203, 213, 225))
$brMuted = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(148, 163, 184))
$brDim   = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(100, 116, 139))
$brDanger= New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(239, 68, 68))
$brSoft  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(226, 232, 240))

$sfLeft = New-Object System.Drawing.StringFormat
$sfLeft.Alignment = [System.Drawing.StringAlignment]::Near
$sfLeft.LineAlignment = [System.Drawing.StringAlignment]::Center

$sfCenter = New-Object System.Drawing.StringFormat
$sfCenter.Alignment = [System.Drawing.StringAlignment]::Center
$sfCenter.LineAlignment = [System.Drawing.StringAlignment]::Center

$sfRight = New-Object System.Drawing.StringFormat
$sfRight.Alignment = [System.Drawing.StringAlignment]::Far
$sfRight.LineAlignment = [System.Drawing.StringAlignment]::Center

# --- Top Banner Card (Y: 16, H: 72) ---
$bannerY = $margin
$bannerH = (S 72)
$bannerRect = New-Object System.Drawing.Rectangle($margin, $bannerY, ($w - 2 * $margin), $bannerH)
Draw-RoundedCard $g $bannerRect $cardBg $cardBorder (S 8)

if ($Populated) {
    $procTitle = "Cyberpunk2077.exe (PID: 14280)"
    $procSub = "Duration: 14:32  |  Total Frames: 124,850  |  Stutters Detected: 18"
} else {
    $procTitle = "No Active Capture Session"
    $procSub = "Session idle $emdash click Start to begin monitoring"
}

$g.DrawString($procTitle, $fontTitle, $brPri, (New-Object System.Drawing.RectangleF(($margin + (S 16)), ($bannerY + (S 12)), ($w - (S 200)), (S 24))), $sfLeft)
$g.DrawString($procSub, $fontRegular, $brMuted, (New-Object System.Drawing.RectangleF(($margin + (S 16)), ($bannerY + (S 38)), ($w - 2 * $margin - (S 32)), (S 22))), $sfLeft)

# --- Metrics Grid (5 Cards side-by-side) ---
$gridY = $bannerY + $bannerH + (S 12)
$gridH = (S 76)
$numCards = 5
$cardGap = (S 8)
$totalCardW = ($w - 2 * $margin - ($numCards - 1) * $cardGap)
$cardW = [int]($totalCardW / $numCards)

$cards = if ($Populated) {
    @(
        @{ Label = "Avg FPS"; Val = "142 FPS"; Brush = $brLabel },
        @{ Label = "1% Low FPS"; Val = "98 FPS"; Brush = $brLabel },
        @{ Label = "0.1% Low FPS"; Val = "62 FPS"; Brush = $brLabel },
        @{ Label = "Net Stall"; Val = "142.5 ms"; Brush = $brDanger },
        @{ Label = "Worst Stutter"; Val = "68.4 ms"; Brush = $brDanger }
    )
} else {
    @(
        @{ Label = "Avg FPS"; Val = "$emdash"; Brush = $brMuted },
        @{ Label = "1% Low FPS"; Val = "$emdash"; Brush = $brMuted },
        @{ Label = "0.1% Low FPS"; Val = "$emdash"; Brush = $brMuted },
        @{ Label = "Net Stall"; Val = "$emdash"; Brush = $brMuted },
        @{ Label = "Worst Stutter"; Val = "$emdash"; Brush = $brMuted }
    )
}

for ($i = 0; $i -lt $numCards; $i++) {
    $cx = $margin + $i * ($cardW + $cardGap)
    $cRect = New-Object System.Drawing.Rectangle($cx, $gridY, $cardW, $gridH)
    Draw-RoundedCard $g $cRect $cardBg $cardBorder (S 6)

    $lblRect = New-Object System.Drawing.RectangleF(($cx + (S 8)), ($gridY + (S 8)), ($cardW - (S 16)), (S 18))
    $g.DrawString($cards[$i].Label, $fontSmall, $brMuted, $lblRect, $sfCenter)

    $valRect = New-Object System.Drawing.RectangleF(($cx + (S 8)), ($gridY + (S 28)), ($cardW - (S 16)), (S 38))
    $g.DrawString($cards[$i].Val, $fontMetric, $cards[$i].Brush, $valRect, $sfCenter)
}

# --- Presentation Cadence Card ---
$cadenceY = $gridY + $gridH + (S 12)
$cadenceH = (S 82)
$cadRect = New-Object System.Drawing.Rectangle($margin, $cadenceY, ($w - 2 * $margin), $cadenceH)
Draw-RoundedCard $g $cadRect $cardBg $cardBorder (S 8)

$g.DrawString("Presentation Cadence", $fontBold, $brPri, (New-Object System.Drawing.RectangleF(($margin + (S 16)), ($cadenceY + (S 10)), ($w - (S 32)), (S 18))), $sfLeft)

if ($Populated) {
    $colWCad = [int](($w - 2 * $margin - (S 32)) / 3)
    $r1Y = $cadenceY + (S 32)
    $r2Y = $cadenceY + (S 55)

    $g.DrawString("Baseline Cadence: 7.0 ms (142 FPS)", $fontRegular, $brSoft, (New-Object System.Drawing.RectangleF(($margin + (S 16)), $r1Y, $colWCad, (S 20))), $sfLeft)
    $g.DrawString("Active Profile: Auto-Adaptive", $fontRegular, $brSoft, (New-Object System.Drawing.RectangleF(($margin + (S 16) + $colWCad), $r1Y, $colWCad, (S 20))), $sfLeft)
    $g.DrawString("Binding Floor: $gte 14.0 ms (Dynamic)", $fontRegular, $brSoft, (New-Object System.Drawing.RectangleF(($margin + (S 16) + 2 * $colWCad), $r1Y, $colWCad, (S 20))), $sfLeft)

    $g.DrawString("Dynamic Trigger: $gte 14.0 ms (2.0x)", $fontRegular, $brSoft, (New-Object System.Drawing.RectangleF(($margin + (S 16)), $r2Y, $colWCad, (S 20))), $sfLeft)
    $g.DrawString("Static Threshold: $gte 21.0 ms (20.0 ms + 5% margin)", $fontRegular, $brSoft, (New-Object System.Drawing.RectangleF(($margin + (S 16) + $colWCad), $r2Y, (2 * $colWCad), (S 20))), $sfLeft)
} else {
    $g.DrawString("Select a target process on the main dashboard to track presentation cadence.", $fontRegular, $brMuted, (New-Object System.Drawing.RectangleF(($margin + (S 16)), ($cadenceY + (S 36)), ($w - (S 32)), (S 24))), $sfLeft)
}

# --- Culprit Attribution Card (Top Stutter Causes) ---
$tableY = $cadenceY + $cadenceH + (S 12)
$btnH = (S 34)
$tableH = $h - $margin - $btnH - (S 12) - $tableY
$tableRect = New-Object System.Drawing.Rectangle($margin, $tableY, ($w - 2 * $margin), $tableH)
Draw-RoundedCard $g $tableRect $cardBg $cardBorder (S 8)

$g.DrawString("Top Stutter Causes", $fontBold, $brPri, (New-Object System.Drawing.RectangleF(($margin + (S 16)), ($tableY + (S 12)), ($w - (S 32)), (S 20))), $sfLeft)

# Columns setup
$colDefs = @(
    @{ Header = "HYPOTHESIS"; Width = 0; Align = $sfLeft },
    @{ Header = "TOP DRIVER"; Width = (S 180); Align = $sfLeft },
    @{ Header = "COUNT"; Width = (S 80); Align = $sfCenter },
    @{ Header = "TOTAL STALL"; Width = (S 115); Align = $sfRight },
    @{ Header = "STALL %"; Width = (S 75); Align = $sfRight }
)

$hdrY = $tableY + (S 38)
$hdrH = (S 22)
$innerLeft = $margin + (S 12)
$innerRight = $w - $margin - (S 12)
$pad = (S 6)

$fixedTotal = 0
for ($c = 1; $c -lt $colDefs.Count; $c++) { $fixedTotal += $colDefs[$c].Width }
$colDefs[0].Width = ($innerRight - $innerLeft) - $fixedTotal

$curX = $innerLeft
$colBounds = @()
foreach ($col in $colDefs) {
    $colBounds += @{ Left = $curX; Right = ($curX + $col.Width); Align = $col.Align }
    $rcHdr = New-Object System.Drawing.RectangleF(($curX + $pad), $hdrY, ($col.Width - 2 * $pad), $hdrH)
    $g.DrawString($col.Header, $fontSmall, $brDim, $rcHdr, $col.Align)
    $curX += $col.Width
}

# Header horizontal divider
$divPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(40, 48, 66), 1.0)
$g.DrawLine($divPen, ($tableRect.Left + (S 12)), ($hdrY + $hdrH + (S 2)), ($tableRect.Right - (S 12)), ($hdrY + $hdrH + (S 2)))

# Rows
if ($Populated) {
    $rows = @(
        @("External Contention (Kernel DPC / ISR)", "nvlddmkm.sys", "8", "68.4 ms", "48.0%"),
        @("Game Engine Execution Stall", "Cyberpunk2077.exe", "5", "42.1 ms", "29.5%"),
        @("DWM Presentation / Compositor Lag", "dwm.exe", "3", "21.0 ms", "14.7%"),
        @("Disk I/O Stall (Pagefile / Asset Stream)", "storport.sys", "2", "11.0 ms", "7.8%")
    )

    $rowY = $hdrY + (S 26)
    $rowH = (S 24)
    $altBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(34, 40, 54))

    for ($r = 0; $r -lt $rows.Count; $r++) {
        if ($r % 2 -eq 1) {
            $rRect = New-Object System.Drawing.Rectangle(($tableRect.Left + (S 8)), $rowY, ($tableRect.Width - (S 16)), $rowH)
            $g.FillRectangle($altBrush, $rRect)
        }
        # Notice: NO STRIPES DRAWN! Clean uniform rows.
        for ($c = 0; $c -lt $colDefs.Count; $c++) {
            $cb = $colBounds[$c]
            $cellRect = New-Object System.Drawing.RectangleF(($cb.Left + $pad), $rowY, ($cb.Right - $cb.Left - 2 * $pad), $rowH)
            $cellBrush = if ($c -eq 0) { $brPri } else { $brLabel }
            $g.DrawString($rows[$r][$c], $fontRegular, $cellBrush, $cellRect, $cb.Align)
        }
        $rowY += $rowH
    }
    $altBrush.Dispose()
} else {
    $emptyRect = New-Object System.Drawing.RectangleF(($tableRect.Left + (S 16)), ($hdrY + (S 36)), ($tableRect.Width - (S 32)), ($tableH - (S 80)))
    $g.DrawString("No session telemetry recorded yet.", $fontRegular, $brMuted, $emptyRect, $sfCenter)
}
$divPen.Dispose()

# --- Footer Buttons ---
$btnY = $h - $margin - $btnH
$btnCopyW = (S 130)
$btnExportW = (S 120)
$btnResetW = (S 95)
$btnCloseW = (S 95)
$gap = (S 10)

function Draw-Button($text, [int]$bx, [int]$bw, [bool]$isDanger = $false) {
    $bRect = New-Object System.Drawing.Rectangle($bx, $btnY, $bw, $btnH)
    if ($isDanger) {
        $bBg = [System.Drawing.Color]::FromArgb(185, 28, 28)
        $bBorder = [System.Drawing.Color]::FromArgb(220, 38, 38)
        $bTextBr = $brPri
    } else {
        $bBg = [System.Drawing.Color]::FromArgb(28, 33, 46)
        $bBorder = [System.Drawing.Color]::FromArgb(50, 60, 82)
        $bTextBr = $brSoft
    }
    Draw-RoundedCard $g $bRect $bBg $bBorder (S 6)
    $txtRect = New-Object System.Drawing.RectangleF($bx, $btnY, $bw, $btnH)
    $g.DrawString($text, $fontBold, $bTextBr, $txtRect, $sfCenter)
}

$bx = $margin
Draw-Button "Copy Summary" $bx $btnCopyW
$bx += $btnCopyW + $gap
Draw-Button "Export JSON" $bx $btnExportW
$bx += $btnExportW + $gap
Draw-Button "Reset" $bx $btnResetW $true

$closeX = $w - $margin - $btnCloseW
Draw-Button "Close" $closeX $btnCloseW

# Save PNG
$bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)

# Cleanup
$fontTitle.Dispose()
$fontMetric.Dispose()
$fontBold.Dispose()
$fontRegular.Dispose()
$fontSmall.Dispose()
$brPri.Dispose()
$brLabel.Dispose()
$brMuted.Dispose()
$brDim.Dispose()
$brDanger.Dispose()
$brSoft.Dispose()
$g.Dispose()
$bmp.Dispose()

Write-Host "Rendered snapshot to: $OutputPath" -ForegroundColor Green
