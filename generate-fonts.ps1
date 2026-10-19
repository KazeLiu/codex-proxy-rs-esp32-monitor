param([string]$Font = 'C:\Windows\Fonts\MiSans-Regular.ttf')
$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot 'firmware\main'
foreach ($size in 12, 14, 32) {
    $ranges = if ($size -eq 14) {
        '0x20-0x7e,0xa0-0xff,0x2000-0x206f,0x3000-0x303f,0x3400-0x4dbf,0x4e00-0x9fff,0xff00-0xffef'
    } else { '0x20-0x7e' }
    $arguments = @('exec', '--yes', '--package=lv_font_conv', '--', 'lv_font_conv',
        '--font', $Font, '--size', $size, '--bpp', '4', '--range', $ranges,
        '--format', 'lvgl', '--output', (Join-Path $output "cpr_misans_$size.c"))
    if ($size -ne 14) { $arguments += '--no-compress' }
    & npm.cmd @arguments
    if ($LASTEXITCODE -ne 0) { throw "MiSans $size px conversion failed." }
}
