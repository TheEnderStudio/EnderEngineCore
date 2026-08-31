# Compile the NRD 4.18 REBLUR_DIFFUSE_SPECULAR shaders WITHOUT --stripReflection
# so that resource names survive into RDAT and Diligent can create PSOs by name.
#
# Usage: pwsh -File nrd_compile_shaders.ps1 [-NrdRoot <path to NRD-master>] [-OutDir <output dir>]
#
# Output: one .dxil blob per permutation (named <shader>__<defines>.dxil) plus
# a generated DenoisingSubsystemShaders.generated.hpp with the byte arrays.

param(
    [string]$NrdRoot = "",
    [string]$OutDir = "..\..\ThirdParty\nrd\NRDShaders"
)

$ErrorActionPreference = "Stop"
$dxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\dxc.exe"
if (-not (Test-Path $dxc)) { throw "dxc not found: $dxc" }

$shadersDir = Join-Path $NrdRoot "Shaders"
$mathlib = Join-Path $NrdRoot "_Build\_deps\mathlib-src"
if (-not (Test-Path (Join-Path $mathlib "ml.hlsli"))) {
    $mathlib = Join-Path $NrdRoot "_NRD_SDK\_deps\mathlib-src"
}
if (-not (Test-Path (Join-Path $mathlib "ml.hlsli"))) { throw "ml.hlsli not found" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Permutations to compile, matching Shaders.cfg + the defines the runtime passes
# to AddDispatch (so the identifier string equals NRD's FillShaderIdentifier output).
# Each entry: shader file -> list of define sets (define order = runtime order).
$common = @("NRD_SIGNAL=NRD_SIGNAL_BOTH", "NRD_MODE=NRD_MODE_RADIANCE")
$perms = [ordered]@{
    "Clear.cs.hlsl"                        = @(@("FLOAT=0"), @("FLOAT=1"))
    "REBLUR_ClassifyTiles.cs.hlsl"         = @(@())
    "REBLUR_HitDistReconstruction.cs.hlsl" = @(@("MODE_5X5=0"), @("MODE_5X5=1"))
    "REBLUR_PrePass.cs.hlsl"               = @(@())
    "REBLUR_TemporalAccumulation.cs.hlsl"  = @(@())
    "REBLUR_HistoryFix.cs.hlsl"            = @(@())
    "REBLUR_Blur.cs.hlsl"                  = @(@())
    "REBLUR_PostBlur.cs.hlsl"              = @(@("TEMPORAL_STABILIZATION=0"), @("TEMPORAL_STABILIZATION=1"))
    "REBLUR_TemporalStabilization.cs.hlsl" = @(@())
    "REBLUR_SplitScreen.cs.hlsl"           = @(@())
    "REBLUR_Validation.cs.hlsl"            = @(@())
}
# Which of the above take the common signal/mode defines (per Shaders.cfg)
$withCommon = @{
    "REBLUR_HitDistReconstruction.cs.hlsl" = $true
    "REBLUR_PrePass.cs.hlsl"               = $true
    "REBLUR_TemporalAccumulation.cs.hlsl"  = $true
    "REBLUR_HistoryFix.cs.hlsl"            = $true
    "REBLUR_Blur.cs.hlsl"                  = $true
    "REBLUR_PostBlur.cs.hlsl"              = $true
    "REBLUR_TemporalStabilization.cs.hlsl" = $true
    "REBLUR_SplitScreen.cs.hlsl"           = $true
}

$results = @() # @{ identifier; data }
foreach ($file in $perms.Keys) {
    $defSets = $perms[$file]
    if ($defSets.Count -eq 0) { $defSets = @(,@()) }
    foreach ($defSet in $defSets) {
        $runtimeDefines = @()
        if ($withCommon[$file]) { $runtimeDefines += $common }
        $runtimeDefines += $defSet
        $compileDefines = @("NRD_INTERNAL") + $runtimeDefines

        $idParts = @($file) + ($runtimeDefines | ForEach-Object { $_ })
        $identifier = $idParts -join "|"
        $outParts = @($file) + ($runtimeDefines | ForEach-Object { "__" + ($_ -replace '=', '_') })
        $outName = $outParts -join ""
        $outPath = Join-Path $OutDir ($outName + ".dxil")

        $args = @("-E", "main", "-T", "cs_6_0")
        foreach ($d in $compileDefines) { $args += @("-D", $d) }
        $args += @("-I", $shadersDir, "-I", $mathlib)
        $args += @((Join-Path $shadersDir $file), "-Fo", $outPath)

        & $dxc @args 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $outPath)) {
            Write-Error "dxc failed for $identifier"
            & $dxc @args
            exit 1
        }
        $data = [System.IO.File]::ReadAllBytes($outPath)
        Write-Host "OK  [$identifier]  ($($data.Length) bytes)"
        $results += @{ identifier = $identifier; data = $data }
    }
}

# Generate the C++ header with byte arrays + per-shader resource name tables
# (dxcreflect is built by xmake from Tools/nrd/dxcreflect/dxcreflect.cpp)
$dxcreflect = Join-Path $PSScriptRoot "dxcreflect\dxcreflect.exe"
if (-not (Test-Path $dxcreflect)) {
    $repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    $built = Join-Path $repoRoot "Binary\windows-x64-debug\dxcreflect.exe"
    if (Test-Path $built) { $dxcreflect = $built }
    else { throw "dxcreflect.exe not found - build it first with: xmake build dxcreflect (looked at $dxcreflect and $built)" }
}
$env:PATH = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64;" + $env:PATH

$resTable = @() # @{ identifier; cbName; names = string[] }
foreach ($r in $results) {
    $parts = $r.identifier -split "\|"
    $f = $parts[0]
    if ($parts.Count -gt 1) { $defines = $parts[1..($parts.Count - 1)] } else { $defines = @() }
    $oParts = @($f) + ($defines | ForEach-Object { "__" + ($_ -replace '=', '_') })
    $dxilPath = Join-Path $OutDir (($oParts -join "") + ".dxil")
    if (-not (Test-Path $dxilPath)) { throw "missing compiled shader: $dxilPath" }
    $out = & $dxcreflect $dxilPath 2>&1 | Out-String
    $names = @()
    $cbName = ""
    $hasSamplers = $false
    foreach ($m in [regex]::Matches($out, "name='([^']*)' type=(\w+)")) {
        if ($m.Groups[2].Value -eq "CBUFFER") { $cbName = $m.Groups[1].Value }
        elseif ($m.Groups[2].Value -eq "TEXTURE" -or $m.Groups[2].Value -eq "UAV_RWTYPED") { $names += $m.Groups[1].Value }
        elseif ($m.Groups[2].Value -eq "SAMPLER") { $hasSamplers = $true }
    }
    if ($cbName -eq "") { Write-Error "no CB found for $($r.identifier)"; exit 1 }
    $resTable += @{ identifier = $r.identifier; cbName = $cbName; names = $names; hasSamplers = $hasSamplers }
    Write-Host "  resources[$($r.identifier)] cb='$cbName' samplers=$hasSamplers names=$($names -join ',')"
}

$resHppPath = Join-Path $OutDir "NRDShaderResources.generated.hpp"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("// AUTO-GENERATED by Tools/nrd_compile_shaders.ps1 - do not edit manually.")
[void]$sb.AppendLine("#include <cstddef>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace EnderEngine { namespace Rendering { namespace NRDShaders {")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("struct NRDShaderResources { const char* identifier; const char* cbName; bool hasSamplers; const char* const* names; size_t namesNum; };")
[void]$sb.AppendLine("")
$idx = 0
foreach ($r in $resTable) {
    $sym = "g_Names$idx"
    [void]$sb.AppendLine("static const char* const $sym[] = {")
    foreach ($n in $r.names) {
        $esc = $n -replace '\\', '\\\\' -replace '"', '\"'
        [void]$sb.AppendLine("    `"$esc`",")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
    $idx++
}
[void]$sb.AppendLine("inline const NRDShaderResources* GetShaderResources(size_t& count) {")
[void]$sb.AppendLine("    static const NRDShaderResources res[] = {")
$idx = 0
foreach ($r in $resTable) {
    $esc = $r.identifier -replace '\\', '\\\\' -replace '"', '\"'
    $cbEsc = $r.cbName -replace '\\', '\\\\' -replace '"', '\"'
    $samp = if ($r.hasSamplers) { "true" } else { "false" }
    [void]$sb.AppendLine("        { `"$esc`", `"$cbEsc`", $samp, g_Names$idx, $($r.names.Count) },")
    $idx++
}
[void]$sb.AppendLine("    };")
[void]$sb.AppendLine("    count = $($resTable.Count);")
[void]$sb.AppendLine("    return res;")
[void]$sb.AppendLine("}")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("} } }")
[System.IO.File]::WriteAllText($resHppPath, $sb.ToString())
Write-Host "Generated $resHppPath with $($resTable.Count) resource tables"

# ---- Generate the C++ header with the byte arrays ----
$hppPath = Join-Path $OutDir "DenoisingSubsystemShaders.generated.hpp"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("// AUTO-GENERATED by Tools/nrd_compile_shaders.ps1 - do not edit manually.")
[void]$sb.AppendLine("// Compiled from NRD 4.18 shader sources WITHOUT --stripReflection so resource")
[void]$sb.AppendLine("// names survive into RDAT (the NRD.dll blobs are name-stripped and Diligent")
[void]$sb.AppendLine("// cannot build PSOs from them - it binds resources by name).")
[void]$sb.AppendLine("#include <cstddef>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace EnderEngine { namespace Rendering { namespace NRDShaders {")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("struct NRDShaderBlob { const char* identifier; const unsigned char* data; size_t size; };")
[void]$sb.AppendLine("")
$idx = 0
foreach ($r in $results) {
    $sym = "g_Shader$idx"
    [void]$sb.AppendLine("static const unsigned char $sym[] = {")
    $line = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $r.data.Length; $i++) {
        [void]$line.Append($r.data[$i])
        [void]$line.Append(",")
        if ($line.Length -gt 100) {
            [void]$sb.AppendLine("    " + $line.ToString())
            $line.Clear() | Out-Null
        }
    }
    if ($line.Length -gt 0) { [void]$sb.AppendLine("    " + $line.ToString()) }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
    $idx++
}
[void]$sb.AppendLine("inline const NRDShaderBlob* GetShaderBlobs(size_t& count) {")
[void]$sb.AppendLine("    static const NRDShaderBlob blobs[] = {")
$idx = 0
foreach ($r in $results) {
    $esc = $r.identifier -replace '\\', '\\\\' -replace '"', '\"'
    [void]$sb.AppendLine("        { `"$esc`", g_Shader$idx, sizeof(g_Shader$idx) },")
    $idx++
}
[void]$sb.AppendLine("    };")
[void]$sb.AppendLine("    count = $($results.Count);")
[void]$sb.AppendLine("    return blobs;")
[void]$sb.AppendLine("}")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("} } }")
[System.IO.File]::WriteAllText($hppPath, $sb.ToString())
Write-Host "Generated $hppPath with $($results.Count) shaders"
