# Set working directory to the script's folder
Set-Location -Path $PSScriptRoot

# Auto-detect default Repomix outputs or custom codebase.txt
$candidateFiles = @("repomix-output.xml", "repomix-output.txt", "codebase.txt")
$inputFile = $null

foreach ($cand in $candidateFiles) {
    if (Test-Path (Join-Path $PSScriptRoot $cand)) {
        $inputFile = $cand
        break
    }
}

$maxUploadFiles = 10      # Hard limit: web AI max file upload count
$targetPartSize = 150KB   # Soft target: ideal max size per file

if (-not $inputFile) {
    Write-Host "Error: No Repomix output found (checked: $($candidateFiles -join ', ')) in $PSScriptRoot" -ForegroundColor Red
    Write-Host "Make sure you run Repomix first (e.g., 'npx repomix')." -ForegroundColor Yellow
    return
}

Write-Host "Reading $inputFile..." -ForegroundColor Cyan
$raw = Get-Content (Join-Path $PSScriptRoot $inputFile) -Raw

# 1. Extract Header (Summary + Directory Tree before <files>)
$header = ""
if ($raw -match '(?s)^(.*?)<files>') {
    $header = $matches[1].Trim()
}

# 2. Extract all <file> blocks
$fileMatches = [regex]::Matches($raw, '(?s)<file path=".*?">.*?</file>')
if ($fileMatches.Count -eq 0) {
    Write-Host "No <file> blocks found in $inputFile" -ForegroundColor Red
    return
}

# 3. Calculate total size & determine optimal part count (capped at 10)
$fileSizes = @($fileMatches | ForEach-Object { [System.Text.Encoding]::UTF8.GetByteCount($_.Value) })
$totalSize = ($fileSizes | Measure-Object -Sum).Sum

$neededParts = [math]::Ceiling($totalSize / $targetPartSize)
$numParts    = [math]::Min($neededParts, $maxUploadFiles)
$numParts    = [math]::Min($numParts, $fileMatches.Count)
$numParts    = [math]::Max($numParts, 1)

$idealSizePerPart = $totalSize / $numParts

Write-Host "Total Size: $([math]::Round($totalSize/1KB, 1)) KB across $($fileMatches.Count) files." -ForegroundColor Cyan
Write-Host "Splitting into $numParts balanced part(s) (Limit: $maxUploadFiles files max)...`n" -ForegroundColor Cyan

# 4. Clean up any previous codebase_part_*.txt files
Get-ChildItem -Path $PSScriptRoot -Filter "codebase_part_*.txt" | Remove-Item -Force

# 5. Partition files evenly across parts using optimal linear partitioning
$chunks = [System.Collections.Generic.List[System.Collections.Generic.List[string]]]::new()

if ($numParts -le 1 -or $fileMatches.Count -le 1) {
    $singleChunk = [System.Collections.Generic.List[string]]::new()
    foreach ($m in $fileMatches) {
        $singleChunk.Add($m.Value)
    }
    $chunks.Add($singleChunk)
} else {
    $n = $fileMatches.Count
    $k = $numParts

    # Prefix sums
    $prefix = New-Object 'double[]' ($n + 1)
    for ($i = 1; $i -le $n; $i++) {
        $prefix[$i] = $prefix[$i - 1] + $fileSizes[$i - 1]
    }

    $table = New-Object 'double[,]' ($n + 1), ($k + 1)
    $solution = New-Object 'int[,]' ($n + 1), ($k + 1)

    for ($i = 1; $i -le $n; $i++) {
        $table[$i, 1] = $prefix[$i]
    }
    for ($j = 1; $j -le $k; $j++) {
        $table[1, $j] = $fileSizes[0]
    }

    for ($i = 2; $i -le $n; $i++) {
        for ($j = 2; $j -le $k; $j++) {
            $table[$i, $j] = [double]::PositiveInfinity
            for ($x = 1; $x -lt $i; $x++) {
                $prev = $table[$x, ($j - 1)]
                $diff = $prefix[$i] - $prefix[$x]
                $cost = [math]::Max($prev, $diff)
                if ($cost -lt $table[$i, $j]) {
                    $table[$i, $j] = $cost
                    $solution[$i, $j] = $x
                }
            }
        }
    }

    # Reconstruct partition boundaries
    $boundaries = [System.Collections.Generic.List[int]]::new()
    $currN = $n
    $currK = $k
    while ($currK -gt 1) {
        $splitIdx = $solution[$currN, $currK]
        $boundaries.Add($splitIdx)
        $currN = $splitIdx
        $currK--
    }
    $boundaries.Reverse()

    $startIdx = 0
    foreach ($b in $boundaries) {
        $chunk = [System.Collections.Generic.List[string]]::new()
        for ($idx = $startIdx; $idx -lt $b; $idx++) {
            $chunk.Add($fileMatches[$idx].Value)
        }
        $chunks.Add($chunk)
        $startIdx = $b
    }
    # Final chunk
    $chunk = [System.Collections.Generic.List[string]]::new()
    for ($idx = $startIdx; $idx -lt $n; $idx++) {
        $chunk.Add($fileMatches[$idx].Value)
    }
    $chunks.Add($chunk)
}

# 6. Write part files with header and part labels
for ($i = 0; $i -lt $chunks.Count; $i++) {
    $partNum = $i + 1
    $partContent = $chunks[$i] -join "`r`n`r`n"
    
    $output = @"
$header

<!-- Codebase Split: Part $partNum of $($chunks.Count) -->
<files>
$partContent
</files>
"@
    
    $outFile = Join-Path $PSScriptRoot "codebase_part_$partNum.txt"
    $output | Set-Content $outFile -Encoding UTF8
    
    $sizeKb = [math]::Round((Get-Item $outFile).Length / 1KB, 1)
    Write-Host " [OK] Created codebase_part_$partNum.txt ($($chunks[$i].Count) files, ${sizeKb} KB)" -ForegroundColor Green
}

Write-Host "`nDone! All parts generated successfully." -ForegroundColor Green
