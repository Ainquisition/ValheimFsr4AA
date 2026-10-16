param([string]$DependencyRoot = (Join-Path $PSScriptRoot '../../work'))
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $DependencyRoot 'OptiScaler'))
$lock = Get-Content "$PSScriptRoot/sources.lock.json" -Raw | ConvertFrom-Json
function Run([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed ($LASTEXITCODE)" }
}
$git = @('-c','http.sslBackend=openssl','-c',"safe.directory=$($repository.Replace('\','/'))",'-C',$repository)
if ((& git @git rev-parse HEAD) -ne $lock.OptiScaler) { throw 'OptiScaler commit mismatch; run build.ps1 first' }
Run git ($git + @('diff','--exit-code','HEAD','--'))
Run git ($git + @('submodule','update','--init','--depth','1','--jobs','4'))
$submodules = & git @git submodule status
if ($LASTEXITCODE -ne 0 -or ($submodules | Where-Object {$_ -match '^[+\-U]'})) { throw 'Submodule commit mismatch' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC Build Tools missing' }
# This host has only v145. The upstream project defaults to v143; preserve its source and override at invocation.
$msbuild = "$vs/MSBuild/Current/Bin/MSBuild.exe"
$toolset = if (Test-Path "$vs/MSBuild/Microsoft/VC/v180/Platforms/x64/PlatformToolsets/v145") { 'v145' } else { 'v143' }
Run $msbuild @("$repository/OptiScaler.sln",'/m:4','/p:Configuration=Release','/p:Platform=x64',"/p:PlatformToolset=$toolset",'/p:TrackFileAccess=false','/v:minimal','/fl',"/flp:logfile=$PSScriptRoot/evidence/optiscaler-build.log;verbosity=normal")
$runtime = "$repository/x64/Release/a"
if (!(Test-Path "$runtime/OptiScaler.dll")) { throw 'OptiScaler build artifact missing' }
# Repair upstream post-build packaging omissions. Do not change upstream source or game files.
Copy-Item "$repository/LICENSE" "$runtime/Licenses/OptiScaler-GPL-3.0.txt"
Copy-Item "$repository/external/FidelityFX-SDK/LICENSE.txt" "$runtime/Licenses/FidelityFX-v1.txt"
Copy-Item "$repository/external/FidelityFX-SDK-v2/docs/license.md" "$runtime/Licenses/FidelityFX-v2.md"
@{commit=$lock.OptiScaler; toolset=$toolset; submodules=@($submodules); binaries=@(Get-ChildItem $runtime -Recurse -Filter '*.dll' | ForEach-Object {
    @{path=$_.FullName.Substring($runtime.Length + 1); sha256=(Get-FileHash $_.FullName).Hash; version=$_.VersionInfo.ProductVersion}
})} | ConvertTo-Json -Depth 5 | Set-Content "$PSScriptRoot/evidence/optiscaler-runtime.json"
Write-Host "Built pinned OptiScaler: $runtime. No game files changed."
& "$PSScriptRoot/stage-runtime.ps1" -DependencyRoot $DependencyRoot
