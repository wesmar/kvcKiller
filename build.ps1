# KvcKiller - deterministic Release x64 build script

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$NoBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectFile = Join-Path $ProjectRoot "src\KvcKiller.vcxproj"
$OutputDir   = Join-Path $ProjectRoot "bin"
$OutputExe   = Join-Path $OutputDir "KvcKiller.exe"

$TransientDirs = @(
    (Join-Path $ProjectRoot ".vs"),
    (Join-Path $ProjectRoot "obj"),
    (Join-Path $ProjectRoot "x64"),
    (Join-Path $ProjectRoot "src\obj"),
    (Join-Path $ProjectRoot "src\x64")
)

function Resolve-MSBuild {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $resolved = Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path -LiteralPath $resolved) {
                return $resolved
            }
        }
    }

    throw "MSBuild not found. Install Visual Studio with Desktop C++ workload."
}

function Get-PresentTransientDirs {
    $present = @()
    foreach ($dir in $TransientDirs) {
        if (Test-Path -LiteralPath $dir) {
            $present += $dir
        }
    }
    return @($present)
}

function Remove-TransientDirs {
    $present = @(Get-PresentTransientDirs)
    if ($present.Length -eq 0) {
        Write-Host "[*] Build state already clean." -ForegroundColor DarkGray
        return
    }

    Write-Host "[*] Removing transient build directories..." -ForegroundColor Yellow
    foreach ($dir in $present) {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction Stop
        Write-Host "  [-] Removed: $dir" -ForegroundColor DarkGray
    }
}

function Assert-CleanLayout {
    $remaining = @(Get-PresentTransientDirs)
    if ($remaining.Length -gt 0) {
        throw ("Failed to clean build directories: " + ($remaining -join ", "))
    }
}

function Assert-ProjectExists {
    if (-not (Test-Path -LiteralPath $ProjectFile)) {
        throw "Project file not found: $ProjectFile"
    }
}

function Assert-OutputUnlocked {
    if (-not (Test-Path -LiteralPath $OutputExe)) {
        return
    }

    try {
        $stream = [System.IO.File]::Open($OutputExe,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
        $stream.Close()
    } catch {
        throw "Output executable is locked: $OutputExe. Close the running KvcKiller instance and retry."
    }
}

function Invoke-Build {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MSBuildPath
    )

    if (-not (Test-Path -LiteralPath $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir | Out-Null
    }

    $arguments = @(
        $ProjectFile
        "/t:Build"
        "/p:Configuration=Release;Platform=x64"
        "/m"
        "/nr:false"
        "/v:minimal"
    )

    Write-Host "[*] Building KvcKiller (Release x64)..." -ForegroundColor Cyan
    & $MSBuildPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}

try {
    Assert-ProjectExists
    $msbuild = Resolve-MSBuild
    Write-Host "[*] Using MSBuild: $msbuild" -ForegroundColor Cyan

    Remove-TransientDirs
    Assert-CleanLayout

    if ($NoBuild) {
        Write-Host "[+] Clean completed." -ForegroundColor Green
        exit 0
    }

    Assert-OutputUnlocked
    Invoke-Build -MSBuildPath $msbuild

    if (-not (Test-Path -LiteralPath $OutputExe)) {
        throw "Build completed but output executable was not produced: $OutputExe"
    }

    $file = Get-Item -LiteralPath $OutputExe
    Write-Host "[+] Build successful." -ForegroundColor Green
    Write-Host "[+] Output: $($file.FullName)" -ForegroundColor Green
    Write-Host ("[+] Size: {0:N2} KB" -f ($file.Length / 1KB)) -ForegroundColor Green
    
    # Set deterministic timestamps for reproducibility and anti-forensics
    $timestamp = Get-Date "2030-01-01 00:00:00"
    $file.CreationTime = $timestamp
    $file.LastWriteTime = $timestamp
    Write-Host "[+] Timestamps set to: 2030-01-01 00:00:00" -ForegroundColor Gray
    
    exit 0
}
catch {
    Write-Host "[!] $_" -ForegroundColor Red
    exit 1
}
