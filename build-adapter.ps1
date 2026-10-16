param(
    [string]$Game = 'D:\SteamLibrary\steamapps\common\Valheim',
    [string]$Core = "$env:APPDATA\Thunderstore Mod Manager\DataFolder\Valheim\profiles\Default\BepInEx\core",
    [string]$DependencyRoot = (Join-Path $PSScriptRoot '../../work')
)
$ErrorActionPreference = 'Stop'
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
$scratch = "$DependencyRoot/ValheimFsr4AA-adapter-build"
New-Item -ItemType Directory -Force $scratch | Out-Null
function Run([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed ($LASTEXITCODE)" }
}
$lock = Get-Content "$PSScriptRoot/sources.lock.json" -Raw | ConvertFrom-Json
foreach ($name in 'OptiScaler','NativeRenderingPlugin') {
    if ((& git -C "$DependencyRoot/$name" rev-parse HEAD) -ne $lock.$name) { throw "Dependency mismatch: $name" }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x64 tools missing' }
$commands = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /LD /DVF_NGX_PROBE /DVF_LIVE_ADAPTER /I"$DependencyRoot/NativeRenderingPlugin/PluginSource/source/Unity" /I"$DependencyRoot/OptiScaler/external/vulkan/include" /I"$DependencyRoot/OptiScaler/external/nvngx_dlss_sdk" "$PSScriptRoot/src/Bridge.cpp" /Fo"$scratch/Bridge.obj" /Fe"$scratch/ValheimFsr4AA.Native.dll" /link /IMPLIB:"$scratch/Bridge.lib"
exit /b %errorlevel%
"@
Set-Content "$scratch/build.cmd" $commands -Encoding ascii
Run $env:ComSpec @('/d','/c',"$scratch/build.cmd")
$csproj = (Get-Content "$PSScriptRoot/src/ValheimFsr4AA.csproj" -Raw).Replace('Include="Plugin.cs"',('Include="'+$PSScriptRoot+'/src/Adapter.cs"')).Replace('<AssemblyName>ValheimFsr4AA.Diagnostics</AssemblyName>','<AssemblyName>ValheimFsr4AA.Adapter</AssemblyName>')
$reference = '<Reference Include="assembly_postprocessing"><HintPath>$(GameManaged)/assembly_postprocessing.dll</HintPath><Private>false</Private></Reference>'
$csproj.Replace('</ItemGroup>',"$reference</ItemGroup>") | Set-Content "$scratch/Adapter.csproj"
Run dotnet @('build',"$scratch/Adapter.csproj",'-c','Release',"-p:GameManaged=$Game/valheim_Data/Managed","-p:BepInExCore=$Core","-p:BaseIntermediateOutputPath=$scratch/obj/",'-o',"$scratch/managed")
New-Item -ItemType Directory -Force "$PSScriptRoot/bin-adapter" | Out-Null
Copy-Item "$scratch/ValheimFsr4AA.Native.dll" "$PSScriptRoot/bin-adapter"
Copy-Item "$scratch/managed/ValheimFsr4AA.Adapter.dll" "$PSScriptRoot/bin-adapter"
Get-FileHash "$PSScriptRoot/bin-adapter/*.dll" | Select-Object Path,Hash | ConvertTo-Json | Set-Content "$PSScriptRoot/evidence/adapter-hashes.json"
Write-Host 'Adapter candidate built. No installed game/profile files changed. Camera mapping requires a real world check.'
