param([string]$DependencyRoot = (Join-Path $PSScriptRoot '../../work'))
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath("$DependencyRoot/OptiScaler")
$lock = Get-Content "$PSScriptRoot/sources.lock.json" -Raw | ConvertFrom-Json
if ((& git -C $repo rev-parse HEAD) -ne $lock.OptiScaler) { throw 'OptiScaler source commit mismatch' }
$built = "$repo/x64/Release/a"
$target = "$PSScriptRoot/runtime-candidate"
$files = [ordered]@{
    'OptiScaler.dll' = "$built/OptiScaler.dll"
    'OptiScaler/amd_fidelityfx_loader_dx12.dll' = "$built/OptiScaler/amd_fidelityfx_loader_dx12.dll"
    'OptiScaler/amd_fidelityfx_upscaler_dx12.dll' = "$built/OptiScaler/amd_fidelityfx_upscaler_dx12.dll"
    'OptiScaler/D3D12_OptiScaler/D3D12Core.dll' = "$built/OptiScaler/D3D12_OptiScaler/D3D12Core.dll"
    'Licenses/OptiScaler-GPL-3.0.txt' = "$repo/LICENSE"
    'Licenses/FidelityFX-v1.txt' = "$repo/external/FidelityFX-SDK/LICENSE.txt"
    'Licenses/FidelityFX-v2.md' = "$repo/external/FidelityFX-SDK-v2/docs/license.md"
    'Licenses/DirectX_LICENSE.txt' = "$repo/external/directx_agility_sdk/LICENSE.txt"
}
foreach ($name in $files.Keys) {
    New-Item -ItemType Directory -Force (Split-Path "$target/$name" -Parent) | Out-Null
    Copy-Item -LiteralPath $files[$name] -Destination "$target/$name"
}
$ini = Get-Content "$repo/OptiScaler.ini" -Raw
$ini = $ini.Replace('VulkanUpscaler=auto','VulkanUpscaler=ffx_12').Replace('LogToFile=auto','LogToFile=true').Replace('LogLevel=auto','LogLevel=1')
$ini = $ini.Replace('UpscalerIndex=auto','UpscalerIndex=0').Replace('FsrAgilitySDKUpgrade=auto','FsrAgilitySDKUpgrade=true').Replace('Fsr4EnableWatermark=auto','Fsr4EnableWatermark=true')
$ini = $ini.Replace('FGInput=auto','FGInput=nofg').Replace('FGOutput=auto','FGOutput=nofg')
$ini = [regex]::Replace($ini,'(?ms)(\[FrameGen\].*?^Enabled=)auto','${1}false')
$ini = [regex]::Replace($ini,'(?ms)(\[DLSS\].*?^Enabled=)auto','${1}false')
Set-Content "$target/OptiScaler.ini" $ini
@{commit=$lock.OptiScaler; files=@(@($files.Keys) + @('OptiScaler.ini') | ForEach-Object {
    @{path=$_;sha256=(Get-FileHash "$target/$_").Hash.ToLowerInvariant()}
})} | ConvertTo-Json -Depth 4 | Set-Content "$target/manifest.json"
Write-Host "Staged minimal pinned runtime: $target. Game files unchanged."
