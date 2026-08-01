param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$RomPath,

    [Parameter(Position = 1)]
    [string]$OutputPath
)

$romPath = (Resolve-Path -LiteralPath $RomPath).Path

if (-not $OutputPath) {
    $projectRoot = Split-Path -Parent $PSScriptRoot
    $OutputPath = Join-Path $projectRoot "Core\Src\gb_rom.c"
}

$outputPath = [System.IO.Path]::GetFullPath($OutputPath)
$rom = [System.IO.File]::ReadAllBytes($romPath)

if ($rom.Length -lt 0x150) {
    throw "The file is too small to contain a Game Boy ROM header."
}

$titleBytes = foreach ($index in 0x134..0x142) {
    if ($rom[$index] -lt 0x20 -or $rom[$index] -gt 0x7E) {
        break
    }

    $rom[$index]
}
$title = [System.Text.Encoding]::ASCII.GetString([byte[]]$titleBytes)

$headerChecksum = 0
for ($index = 0x134; $index -le 0x14C; ++$index) {
    $headerChecksum = ($headerChecksum - $rom[$index] - 1) -band 0xFF
}
$storedHeaderChecksum = $rom[0x14D]

[uint64]$globalChecksum = 0
for ($index = 0; $index -lt $rom.Length; ++$index) {
    if ($index -ne 0x14E -and $index -ne 0x14F) {
        $globalChecksum += $rom[$index]
    }
}
$globalChecksum = $globalChecksum -band 0xFFFF
$storedGlobalChecksum = ($rom[0x14E] -shl 8) -bor $rom[0x14F]

$headerStatus = if ($headerChecksum -eq $storedHeaderChecksum) { "OK" } else { "MISMATCH" }
$globalStatus = if ($globalChecksum -eq $storedGlobalChecksum) { "OK" } else { "MISMATCH" }

Write-Host "ROM: $romPath"
Write-Host "Title: $title"
Write-Host "Size: $($rom.Length) bytes"
Write-Host ("Header checksum: stored=0x{0:X2}, calculated=0x{1:X2} ({2})" -f $storedHeaderChecksum, $headerChecksum, $headerStatus)
Write-Host ("Global checksum: stored=0x{0:X4}, calculated=0x{1:X4} ({2})" -f $storedGlobalChecksum, $globalChecksum, $globalStatus)

if ($headerStatus -ne "OK" -or $globalStatus -ne "OK") {
    Write-Warning "The ROM checksum does not match. The original file will not be modified."
}

$builder = [System.Text.StringBuilder]::new($rom.Length * 6 + 128)
[void]$builder.Append("#include `"gb_rom.h`"`r`n`r`n")
[void]$builder.Append("const uint8_t gbRomData[] =`r`n{`r`n")

for ($offset = 0; $offset -lt $rom.Length; $offset += 16) {
    [void]$builder.Append("  ")
    $lineEnd = [System.Math]::Min($offset + 16, $rom.Length)

    for ($index = $offset; $index -lt $lineEnd; ++$index) {
        [void]$builder.Append("0x")
        [void]$builder.Append($rom[$index].ToString("X2"))

        if ($index -lt $rom.Length - 1) {
            [void]$builder.Append(",")
        }

        if ($index -lt $lineEnd - 1) {
            [void]$builder.Append(" ")
        }
    }

    [void]$builder.Append("`r`n")
}

[void]$builder.Append("};`r`n`r`n")
[void]$builder.Append("const uint32_t gbRomSize = sizeof(gbRomData);`r`n")

$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($outputPath, $builder.ToString(), $utf8WithoutBom)

Write-Host "Generated: $outputPath"
