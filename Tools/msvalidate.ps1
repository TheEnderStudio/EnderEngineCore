# Offline DXIL validation of the shader sources embedded in the engine.
#
# Every shader in this project is a string literal compiled at runtime through
# Diligent/dxc, so a typo only shows up when the pipeline is created - which can
# be a black screen or a device hang. This extracts each literal and compiles it
# with the same compiler and profile the engine uses, so shader errors surface at
# build time instead.
#
#   usage: powershell -File Tools/msvalidate.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$dxc = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\dxc.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dxc) { throw 'dxc.exe not found' }
Write-Host "dxc: $dxc"

# source file -> shaders it embeds
$sources = @{
    'Source/Engine/Rendering/MeshShaderSubsystem.cpp' = @(
        @{ name = 'g_MeshAS';           profile = 'as_6_5' },
        @{ name = 'g_MeshMS';           profile = 'ms_6_5' },
        @{ name = 'g_MeshPS';           profile = 'ps_6_5' },
        @{ name = 'g_SceneAS';          profile = 'as_6_5' },
        @{ name = 'g_SceneMS';          profile = 'ms_6_5' },
        @{ name = 'g_ScenePS';          profile = 'ps_6_5' },
        @{ name = 'g_ClusterAS';        profile = 'as_6_5' },
        @{ name = 'g_ClusterMS';        profile = 'ms_6_5' },
        @{ name = 'g_ClusterPS';        profile = 'ps_6_5' },
        @{ name = 'g_ClusterGBufferPS'; profile = 'ps_6_5' }
    )
    'Source/Engine/Rendering/ComputeSubsystem.cpp' = @(
        @{ name = 'g_CullingCS';        profile = 'cs_6_0' }
    )
}

$tmp = Join-Path $env:TEMP ('msvalidate_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
$fail = 0
$count = 0
foreach ($rel in $sources.Keys) {
    $path = Join-Path $root $rel
    if (-not (Test-Path $path)) { Write-Host "MISSING FILE $rel" -ForegroundColor Red; $fail++; continue }
    $src = Get-Content -Raw -Encoding UTF8 $path
    foreach ($t in $sources[$rel]) {
        $count++
        $m = [regex]::Match($src, 'static const char\* ' + $t.name + ' = R"\((?s)(.*?)\)";')
        if (-not $m.Success) { Write-Host ("MISSING  {0}" -f $t.name) -ForegroundColor Red; $fail++; continue }
        $file = Join-Path $tmp ($t.name + '.hlsl')
        Set-Content -Path $file -Value $m.Groups[1].Value -Encoding UTF8 -NoNewline
        $out = & $dxc -E main -T $t.profile -all-resources-bound -Fo (Join-Path $tmp ($t.name + '.dxil')) $file 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("FAIL     {0} ({1})" -f $t.name, $t.profile) -ForegroundColor Red
            $out | ForEach-Object { Write-Host "         $_" }
            $fail++
        } else {
            Write-Host ("ok       {0} ({1})" -f $t.name, $t.profile) -ForegroundColor Green
        }
    }
}
Remove-Item -Recurse -Force $tmp
if ($fail) { Write-Host "$fail of $count shader(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "all $count shaders valid"
