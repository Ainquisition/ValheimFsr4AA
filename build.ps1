param(
    [string]$GamePath = 'D:\SteamLibrary\steamapps\common\Valheim',
    [string]$BepInExCore = "$env:APPDATA\Thunderstore Mod Manager\DataFolder\Valheim\profiles\Default\BepInEx\core",
    [string]$DependencyRoot = (Join-Path $PSScriptRoot '../../work')
)
$ErrorActionPreference = 'Stop'
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
New-Item -ItemType Directory -Force $DependencyRoot | Out-Null
$lock = Get-Content (Join-Path $PSScriptRoot 'sources.lock.json') -Raw | ConvertFrom-Json
foreach ($name in 'OptiScaler','NativeRenderingPlugin') {
    $path = Join-Path $DependencyRoot $name
    $url = if ($name -eq 'OptiScaler') { 'https://github.com/optiscaler/OptiScaler.git' } else { 'https://github.com/Unity-Technologies/NativeRenderingPlugin.git' }
    if (!(Test-Path $path)) {
        Run git @('-c','http.sslBackend=openssl','clone','--no-checkout',$url,$path)
        Run git @('-C',$path,'checkout','--detach',$lock.$name)
    }
    $head = & git -c "safe.directory=$($path.Replace('\','/'))" -C $path rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -ne $lock.$name) { throw "Commit mismatch at $path; expected $($lock.$name). Existing checkout was preserved." }
    Run git @('-c',"safe.directory=$($path.Replace('\','/'))",'-C',$path,'diff','--exit-code','HEAD','--')
}
$opti = Join-Path $DependencyRoot 'OptiScaler'
Run git @('-c','http.sslBackend=openssl','-c',"safe.directory=$($opti.Replace('\','/'))",'-C',$opti,'submodule','update','--init','--depth','1','external/vulkan')
$vk = Join-Path $opti 'external/vulkan'
if ((& git -c "safe.directory=$($vk.Replace('\','/'))" -C $vk rev-parse HEAD) -ne $lock.'Vulkan-Headers') { throw 'Vulkan-Headers commit mismatch' }
Run git @('-c',"safe.directory=$($vk.Replace('\','/'))",'-C',$vk,'diff','--exit-code','HEAD','--')
$managed = Join-Path $GamePath 'valheim_Data/Managed'
foreach ($file in @("$BepInExCore/BepInEx.dll", "$BepInExCore/0Harmony.dll", "$managed/UnityEngine.CoreModule.dll")) {
    if (!(Test-Path $file -PathType Leaf)) { throw "Missing reference: $file" }
}
$scratch = Join-Path $DependencyRoot 'ValheimFsr4AA-build'
$bin = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force "$scratch/native" | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual C++ x64 toolchain not found' }
$unity = "$DependencyRoot/NativeRenderingPlugin/PluginSource/source/Unity"
$commands = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /LD /I"$unity" /I"$vk/include" "$PSScriptRoot/src/Probe.cpp" /Fo"$scratch/native/Probe.obj" /Fe"$scratch/native/ValheimFsr4AA.Native.dll" /link /IMPLIB:"$scratch/native/ValheimFsr4AA.Native.lib"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /I"$unity" /I"$vk/include" "$PSScriptRoot/src/probe_test.cpp" /Fo"$scratch/native/probe_test.obj" /Fe"$scratch/native/probe_test.exe"
if errorlevel 1 exit /b 1
"$scratch/native/probe_test.exe"
exit /b %errorlevel%
"@
$nativeBuild = "$scratch/build-native.cmd"
Set-Content -LiteralPath $nativeBuild -Value $commands -Encoding ascii
Run $env:ComSpec @('/d','/c',$nativeBuild)
Write-Host 'PASS probe_fail_closed (synthetic, no GPU)'
Run dotnet @('build',"$PSScriptRoot/src/ValheimFsr4AA.csproj",'-c','Release',"-p:GameManaged=$managed","-p:BepInExCore=$BepInExCore","-p:BaseIntermediateOutputPath=$scratch/managed/",'-o',"$scratch/managed-bin")
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item -LiteralPath "$scratch/native/ValheimFsr4AA.Native.dll" -Destination $bin
Copy-Item -LiteralPath "$scratch/managed-bin/ValheimFsr4AA.Diagnostics.dll" -Destination $bin
Get-FileHash "$bin/*.dll" -Algorithm SHA256 | Select-Object @{Name='File';Expression={Split-Path $_.Path -Leaf}},Hash | ConvertTo-Json | Set-Content "$PSScriptRoot/evidence/hashes.json"
Get-FileHash "$bin/*.dll" -Algorithm SHA256 | Format-Table -AutoSize
Write-Host 'Built diagnostics only. No game files were changed. FSR processing is not implemented.'
