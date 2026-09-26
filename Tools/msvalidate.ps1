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
    'Source/Engine/Rendering/RayTracingSubsystem.cpp' = @(
        @{ name = 'g_RayQueryCS';       profile = 'cs_6_5'; entry = 'CSMain' },
        @{ name = 'g_DenoiseCS';        profile = 'cs_6_5'; entry = 'CSMain' },
        @{ name = 'g_ComposeVS';        profile = 'vs_6_5' },
        @{ name = 'g_ComposePS';        profile = 'ps_6_5' }
    )
    'Source/Engine/Rendering/RenderSubsystem.cpp' = @(
        @{ name = 'g_VS';               profile = 'vs_6_5' },
        @{ name = 'g_VS_Inst';          profile = 'vs_6_5' },
        @{ name = 'g_VS_Indirect';      profile = 'vs_6_5' },
        @{ name = 'g_VS_ShadowIndirect';profile = 'vs_6_5' },
        @{ name = 'g_VS_Billboard';     profile = 'vs_6_5' },
        @{ name = 'g_PS_Billboard';     profile = 'ps_6_5' },
        @{ name = 'g_VS_Skybox';        profile = 'vs_6_5' },
        @{ name = 'g_PS_Skybox';        profile = 'ps_6_5' },
        @{ name = 'g_PS_SkyboxCube';    profile = 'ps_6_5' },
        @{ name = 'g_PS';               profile = 'ps_6_5' },
        @{ name = 'g_PS_GBuffer';       profile = 'ps_6_5' },
        @{ name = 'g_ResolveCS';        profile = 'cs_6_5'; entry = 'CSMain' }
    )
}

$tmp = Join-Path $env:TEMP ('msvalidate_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
$fail = 0
$warn = 0
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
        $entry = if ($t.entry) { $t.entry } else { 'main' }
        # Run dxc through cmd: PowerShell turns a native command's stderr into a
        # terminating error under ErrorActionPreference=Stop, which used to abort the
        # whole run on a mere warning. Only dxc's exit code decides pass/fail.
        $cmd = '"{0}" -E {1} -T {2} -all-resources-bound -Fo "{3}" "{4}" 2>&1' -f `
            $dxc, $entry, $t.profile, (Join-Path $tmp ($t.name + '.dxil')), $file
        $out = cmd /c $cmd
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host ("FAIL     {0} ({1})" -f $t.name, $t.profile) -ForegroundColor Red
            $out | ForEach-Object { Write-Host "         $_" }
            $fail++
        } elseif ($out -match 'warning') {
            # Valid, but worth surfacing: they are the hints that a shader is doing
            # something implicitly (vector truncation, uninitialised value, ...).
            $warn++
            Write-Host ("ok(warn) {0} ({1})" -f $t.name, $t.profile) -ForegroundColor Yellow
            $out | Where-Object { $_ -match 'warning' } | ForEach-Object { Write-Host "         $_" -ForegroundColor DarkYellow }
        } else {
            Write-Host ("ok       {0} ({1})" -f $t.name, $t.profile) -ForegroundColor Green
        }
    }
}
Remove-Item -Recurse -Force $tmp
if ($fail) { Write-Host "$fail of $count shader(s) failed" -ForegroundColor Red; exit 1 }
if ($warn) { Write-Host "all $count shaders valid ($warn with warnings)" -ForegroundColor Yellow }
else { Write-Host "all $count shaders valid" }
