# Builds RushVR with the Move-controllers patch and puts a play folder next to this script.
#   Needs: Git, and Visual Studio 2022 with "Desktop development with C++", its CMake tools and
#   "C++ Clang tools for Windows". Takes 20 to 60 minutes the first time.
# Started by "Build RushVR.bat". The log is build-log.txt next to this script.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here "source"
$patch = Join-Path $here "0001-Add-Quest-controllers-as-PlayStation-Move-wands.patch"
$repo = "https://github.com/RandomName729/RushofPCVR.git"
$base = "b7dcd7c5decb5eb77089982a14b69faefd01289a"
Start-Transcript -Path (Join-Path $here "build-log.txt") -Force | Out-Null

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed (exit code $LASTEXITCODE)" }
}

try {
    Step "Finding Git and Visual Studio"
    $git = (Get-Command git -ErrorAction SilentlyContinue).Source
    if (-not $git) { throw "Git was not found. Install Git for Windows and run this again." }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio 2022 with C++ tools was not found." }
    $clang = Join-Path $vs "VC\Tools\Llvm\x64\bin\clang-cl.exe"
    if (-not (Test-Path $clang)) { throw "clang-cl was not found: add 'C++ Clang tools for Windows' in the Visual Studio Installer." }
    Write-Host "Visual Studio: $vs"

    # The compiler's environment (vcvars64), taken into this PowerShell.
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
    }
    $env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" +
                (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") + ";" +
                (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja") + ";" + $env:PATH
    foreach ($tool in "cmake", "ninja") {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool was not found: add the C++ CMake tools in the Visual Studio Installer." }
    }

    Step "Getting the RushVR source"
    if (-not (Test-Path (Join-Path $src ".git"))) {
        Run $git @("-c", "core.longpaths=true", "clone", $repo, $src)
    }
    Push-Location $src
    Run $git @("config", "core.longpaths", "true")
    $applied = (& $git log --format=%s -1) -eq "Add Quest controllers as PlayStation Move wands"
    if (-not $applied) {
        Run $git @("checkout", "-B", "move-controllers", $base)
        Step "Applying the Move-controllers patch"
        Run $git @("-c", "user.name=RushVR build", "-c", "user.email=build@localhost", "am", "--3way", $patch)
    }
    Step "Getting the emulator's libraries (a few GB, the longest download)"
    Run $git @("-c", "core.longpaths=true", "submodule", "update", "--init", "--recursive", "--depth", "1", "--jobs", "8")
    Pop-Location

    Step "Building the emulator (the longest step)"
    $emu = Join-Path $src "shadps4-arm64-main"
    $build = Join-Path $emu "Build\x64-Clang-Release"
    Run "cmake" @("-S", $emu, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                  "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
                  "-DENABLE_DISCORD_RPC=OFF", "-DENABLE_UPDATER=OFF")
    Run "cmake" @("--build", $build, "--target", "shadps4")
    $exe = Join-Path $build "shadps4.exe"
    if (-not (Test-Path $exe)) { throw "The build finished but shadps4.exe is not where it was expected: $exe" }

    Step "Putting the play folder together in $here"
    foreach ($dir in "pc-vr\user\input_config", "games", "pc-vr\user\home\1000\savedata",
                     "pc-vr\user\home\1000\trophy", "pc-vr\user\home\1000\inputs") {
        New-Item -ItemType Directory -Force -Path (Join-Path $here $dir) | Out-Null
    }
    Copy-Item -LiteralPath $exe -Destination (Join-Path $here "pc-vr\shadps4.exe") -Force
    Copy-Item -LiteralPath (Join-Path $src "pc-vr\launch.ps1") -Destination (Join-Path $here "pc-vr\") -Force
    if (-not (Test-Path (Join-Path $here "pc-vr\settings.txt"))) {
        Copy-Item -LiteralPath (Join-Path $src "pc-vr\settings.txt") -Destination (Join-Path $here "pc-vr\")
    }
    if (-not (Test-Path (Join-Path $here "pc-vr\user\config.json"))) {
        Copy-Item -LiteralPath (Join-Path $src "pc-vr\template\user\config.json") -Destination (Join-Path $here "pc-vr\user\")
        Copy-Item -Path (Join-Path $src "pc-vr\template\user\input_config\*.ini") -Destination (Join-Path $here "pc-vr\user\input_config\")
    }
    foreach ($file in "Play Rush of Blood.bat", "README.md", "CHANGELOG.md", "LICENSE", "THIRD-PARTY-NOTICES.md") {
        Copy-Item -LiteralPath (Join-Path $src $file) -Destination $here -Force
    }

    Step "Done"
    Write-Host "Start the game with: $(Join-Path $here 'Play Rush of Blood.bat')" -ForegroundColor Green
    Write-Host "Turn on 'Quest controllers as Move wands' in its start-up window to use the Quest controllers."
} catch {
    Write-Host ""
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "The full log is build-log.txt in $here"
} finally {
    Stop-Transcript | Out-Null
}
