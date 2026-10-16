param(
    [string]$Game = 'D:\SteamLibrary\steamapps\common\Valheim',
    [string]$Profile = "$env:APPDATA\Thunderstore Mod Manager\DataFolder\Valheim\profiles\Default"
)
$ErrorActionPreference = 'Stop'
if (Get-Process valheim -ErrorAction SilentlyContinue) { throw 'Valheim already running' }
if (Test-Path "$Game/dxgi.dll") { throw 'Foreign dxgi.dll still present. No file was renamed or replaced.' }
$state = Get-Content "$Game/ValheimFsr4AA-backup-0.2.0/manifest.json" -Raw | ConvertFrom-Json
if ($state.state -ne 'installed') { throw 'Candidate is not installed' }
foreach ($file in $state.files) {
    # OptiScaler UI is allowed to save its own ini; binary identity remains pinned.
    if ($file.path.EndsWith('.dll', [StringComparison]::OrdinalIgnoreCase) -and
        (Get-FileHash -LiteralPath $file.path).Hash.ToLowerInvariant() -ne $file.after) { throw "Installed DLL changed: $($file.path)" }
}
$preloader = "$Profile/BepInEx/core/BepInEx.Preloader.dll"
if (!(Test-Path $preloader)) { throw "Missing profile preloader: $preloader" }
$previous = $env:VF_OPTI_DLL
try {
    $env:VF_OPTI_DLL = [IO.Path]::GetFullPath("$Game/ValheimFsr4AA-runtime/OptiScaler.dll")
    $arguments = @('-force-vulkan','--doorstop-enabled','true','--doorstop-target-assembly',('"'+$preloader+'"'))
    Start-Process -FilePath "$Game/valheim.exe" -WorkingDirectory $Game -ArgumentList $arguments -PassThru
} finally { $env:VF_OPTI_DLL = $previous }
Write-Host 'F9 toggles the candidate adapter; use OptiScaler UI to select FSR. Check the runtime watermark and logs.'
