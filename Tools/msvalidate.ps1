# Offline DXIL validation of the mesh-shader sources embedded in
# Source/Engine/Rendering/MeshShaderSubsystem.cpp.
#   usage: pwsh -File Tools/msvalidate.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'Source/Engine/Rendering/MeshShaderSubsystem.cpp')

$dxc = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\dxc.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dxc) { throw 'dxc.exe not found' }
Write-Host "dxc: $dxc"

$targets = @(
    @{ name = 'g_MeshAS';    profile = 'as_6_5' },
    @{ name = 'g_MeshMS';    profile = 'ms_6_5' },
    @{ name = 'g_MeshPS';    profile = 'ps_6_5' },
    @{ name = 'g_SceneAS';   profile = 'as_6_5' },
    @{ name = 'g_SceneMS';   profile = 'ms_6_5' },
    @{ name = 'g_ScenePS';   profile = 'ps_6_5' },
    @{ name = 'g_ClusterAS'; profile = 'as_6_5' },
    @{ name = 'g_ClusterMS'; profile = 'ms_6_5' },
    @{ name = 'g_ClusterPS'; profile = 'ps_6_5' },
    @{ name = 'g_ClusterGBufferPS'; profile = 'ps_6_5' }
)

$tmp = Join-Path $env:TEMP ('msvalidate_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
$fail = 0
foreach ($t in $targets) {
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
Remove-Item -Recurse -Force $tmp
if ($fail) { Write-Host "$fail shader(s) failed" -ForegroundColor Red; exit 1 }
Write-Host 'all shaders valid'
