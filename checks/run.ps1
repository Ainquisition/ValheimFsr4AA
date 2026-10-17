param(
    [switch]$NgxContext,
    [switch]$InstalledLayout,
    [string]$RuntimePackage,
    [string]$Game = 'D:\SteamLibrary\steamapps\common\Valheim',
    [string]$Core = "$env:APPDATA\Thunderstore Mod Manager\DataFolder\Valheim\profiles\Default\BepInEx\core",
    [string]$DependencyRoot = (Join-Path $PSScriptRoot '../../../work'),
    [string]$Scratch = (Join-Path $PSScriptRoot ('../../../work/CommandProbe-' + [guid]::NewGuid().ToString('N')))
)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$Scratch = [IO.Path]::GetFullPath($Scratch)
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
if (Test-Path $Scratch) { throw 'Scratch must be a new directory; existing files were preserved' }
if (Get-Process valheim -ErrorAction SilentlyContinue) { throw 'Close Valheim before this GPU test' }
if ($InstalledLayout -and !$NgxContext) { throw 'InstalledLayout requires NgxContext' }
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
$stage = "$Scratch/Player"
New-Item -ItemType Directory -Force "$stage/valheim_Data/Plugins/x86_64", "$stage/BepInEx/core", "$stage/BepInEx/config", "$stage/BepInEx/plugins" | Out-Null
foreach ($file in 'valheim.exe','UnityPlayer.dll','steam_appid.txt','winhttp.dll','doorstop_config.ini') { Copy-Item -LiteralPath "$Game/$file" -Destination $stage }
Copy-Item "$Core/*" "$stage/BepInEx/core"
foreach ($file in 'app.info','boot.config','globalgamemanagers.assets','RuntimeInitializeOnLoads.json','ScriptingAssemblies.json','level0','sharedassets0.assets','resources.assets','resources.assets.resS') {
    Copy-Item -LiteralPath "$Game/valheim_Data/$file" -Destination "$stage/valheim_Data"
}
$ggm = "$Game/valheim_Data/globalgamemanagers"
$hash = (Get-FileHash $ggm).Hash.ToLowerInvariant()
if ($hash -eq '5161124394055da500c63e1de8098f62f3e7fb189b53873d758dee21ab37acb6') {
    Run python @("$project/preload.py",$ggm,"$stage/valheim_Data/globalgamemanagers")
} elseif ($hash -eq '4876cd0000e82f11a8a666f7eefa7bfc97c476daad5cfd18a22b9e4a254123d8') {
    Copy-Item -LiteralPath $ggm -Destination "$stage/valheim_Data"
} else { throw 'Unknown globalgamemanagers; original game was not modified' }
New-Item -ItemType Junction -Path "$stage/MonoBleedingEdge" -Target "$Game/MonoBleedingEdge" | Out-Null
foreach ($directory in 'Managed','Resources','StreamingAssets') {
    New-Item -ItemType Junction -Path "$stage/valheim_Data/$directory" -Target "$Game/valheim_Data/$directory" | Out-Null
}
Copy-Item "$Game/valheim_Data/Plugins/x86_64/lib_burst_generated.dll" "$stage/valheim_Data/Plugins/x86_64"
# Omit game runtime initializers; the test plugin also blocks the EntryPoint scene loader.
# Other entry-scene callbacks can still run and log platform errors. Inspect the player logs.
$initializers = Get-Content "$stage/valheim_Data/RuntimeInitializeOnLoads.json" -Raw | ConvertFrom-Json
$initializers.root = @($initializers.root | Where-Object {$_.assemblyName -like 'Unity.*'})
$initializers | ConvertTo-Json -Depth 8 -Compress | Set-Content "$stage/valheim_Data/RuntimeInitializeOnLoads.json"
"[Logging.Console]`nEnabled = false`n[Logging.Disk]`nEnabled = true`nLogLevels = All" | Set-Content "$stage/BepInEx/config/BepInEx.cfg"
$lock = Get-Content "$project/sources.lock.json" -Raw | ConvertFrom-Json
foreach ($name in 'OptiScaler','NativeRenderingPlugin') {
    $repository = Join-Path $DependencyRoot $name
    $head = & git -c "safe.directory=$($repository.Replace('\','/'))" -C $repository rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -ne $lock.$name) { throw "Dependency mismatch: $name; run the normal build with pinned sources first" }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual C++ x64 tools missing' }
$ngxDefine = if ($NgxContext) { '/DVF_NGX_PROBE /DVF_LIVE_ADAPTER' } else { '' }
if ($NgxContext) {
    $runtime = if ($RuntimePackage) { [IO.Path]::GetFullPath($RuntimePackage) } else { "$DependencyRoot/OptiScaler/x64/Release/a" }
    if (!(Test-Path "$runtime/OptiScaler.dll")) { throw 'Build pinned OptiScaler before -NgxContext' }
    $runtimeTarget = if ($InstalledLayout) { "$stage/ValheimFsr4AA-runtime" } else { $stage }
    New-Item -ItemType Directory -Force $runtimeTarget | Out-Null
    Copy-Item "$runtime/OptiScaler.dll" $runtimeTarget
    Copy-Item "$runtime/OptiScaler" $runtimeTarget -Recurse
    $ini = Get-Content "$DependencyRoot/OptiScaler/OptiScaler.ini" -Raw
    if ($InstalledLayout) { $ini = $ini.Replace('OptiDllPath=auto','OptiDllPath=ValheimFsr4AA-runtime/OptiScaler') }
    $ini = $ini.Replace('VulkanUpscaler=auto','VulkanUpscaler=ffx_12').Replace('LogToFile=auto','LogToFile=true').Replace('LogLevel=auto','LogLevel=1')
    $ini = $ini.Replace('UpscalerIndex=auto','UpscalerIndex=0').Replace('FsrAgilitySDKUpgrade=auto','FsrAgilitySDKUpgrade=true').Replace('Fsr4EnableWatermark=auto','Fsr4EnableWatermark=true')
    # Disable calls to NVIDIA runtime and all frame-generation inputs/outputs in this context-only check.
    $ini = $ini.Replace('FGInput=auto','FGInput=nofg').Replace('FGOutput=auto','FGOutput=nofg')
    $ini = [regex]::Replace($ini, '(?ms)(\[FrameGen\].*?^Enabled=)auto', '${1}false')
    $ini = [regex]::Replace($ini, '(?ms)(\[DLSS\].*?^Enabled=)auto', '${1}false')
    Set-Content "$runtimeTarget/OptiScaler.ini" $ini
}
$commands = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /LD $ngxDefine /I"$DependencyRoot/NativeRenderingPlugin/PluginSource/source/Unity" /I"$DependencyRoot/OptiScaler/external/vulkan/include" /I"$DependencyRoot/OptiScaler/external/nvngx_dlss_sdk" "$PSScriptRoot/CommandProbe.cpp" /Fo"$Scratch/CommandProbe.obj" /Fe"$stage/valheim_Data/Plugins/x86_64/ValheimFsr4AA.Native.dll" /link /IMPLIB:"$Scratch/CommandProbe.lib"
exit /b %errorlevel%
"@
Set-Content "$Scratch/build-native.cmd" -Value $commands -Encoding ascii
Run $env:ComSpec @('/d','/c',"$Scratch/build-native.cmd")
(Get-Content "$project/src/ValheimFsr4AA.csproj" -Raw).Replace('Include="Plugin.cs"',('Include="'+$PSScriptRoot+'/CommandProbe.cs"')).Replace('<AssemblyName>ValheimFsr4AA.Diagnostics</AssemblyName>','<AssemblyName>VF.CommandProbe</AssemblyName>') | Set-Content "$Scratch/CommandProbe.csproj"
Run dotnet @('build',"$Scratch/CommandProbe.csproj",'-c','Release',"-p:GameManaged=$Game/valheim_Data/Managed","-p:BepInExCore=$Core","-p:BaseIntermediateOutputPath=$Scratch/obj/",'-o',"$Scratch/bin")
Copy-Item "$Scratch/bin/VF.CommandProbe.dll" "$stage/BepInEx/plugins"
$originalBoot = Get-Content "$stage/valheim_Data/boot.config" -Raw
$baseBoot = [regex]::Replace([regex]::Replace($originalBoot,'(?m)^gfx-enable-gfx-jobs=.*$','gfx-enable-gfx-jobs=1'),'(?m)^gfx-threading-mode=.*$','gfx-threading-mode=6')
$modes = if ($NgxContext) { @('default') } else { @('default','direct','jobs-off') }
foreach ($mode in $modes) {
    $boot = if ($mode -eq 'jobs-off') { $baseBoot.Replace('gfx-enable-gfx-jobs=1','gfx-enable-gfx-jobs=0').Replace('gfx-threading-mode=6','gfx-threading-mode=2') } else { $baseBoot }
    Set-Content "$stage/valheim_Data/boot.config" -Value $boot -NoNewline
    $arguments = @('-force-vulkan','-noaudio','-logFile',('"'+$Scratch+'/'+$mode+'-player.log"'))
    if ($mode -eq 'direct') { $arguments += '-force-gfx-direct' }
    $previous = $env:VF_COMMAND_RESULT
    $previousQueue = $env:VF_QUEUE_ONLY
    $previousOpti = $env:VF_OPTI_DLL
    try {
        $env:VF_COMMAND_RESULT = "$Scratch/$mode-results.log"
        $env:VF_QUEUE_ONLY = if ($NgxContext) { '1' } else { '0' }
        $env:VF_OPTI_DLL = if ($NgxContext -and !$InstalledLayout) { "$stage/OptiScaler.dll" } else { $null }
        $process = Start-Process "$stage/valheim.exe" -WorkingDirectory $stage -ArgumentList $arguments -WindowStyle Hidden -PassThru
    } finally { $env:VF_COMMAND_RESULT = $previous; $env:VF_QUEUE_ONLY = $previousQueue; $env:VF_OPTI_DLL = $previousOpti }
    if (!$process.WaitForExit(40000)) { $process.Kill(); $process.WaitForExit(); throw 'Isolated child timed out' }
    if ($process.ExitCode -ne 0) { throw "Isolated child failed ($($process.ExitCode)); inspect $Scratch" }
    Get-Content "$Scratch/$mode-results.log"
}
Write-Host "Results and test-only binaries: $Scratch. Installed game and profile were not modified."
