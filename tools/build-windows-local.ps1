# Local Windows release build of this checkout, installed into a runnable folder.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-local.ps1 [-Configure]
#
# It configures (first run, or -Configure) and builds the launcher and kyty_emulator as the U59
# release workflow does (.github/workflows/u59-windows-release.yml): LLVM 22.1.3 clang-cl, IPO and
# the bundled PGO profile, from C:\kyty-src19 (the profile names anonymous-namespace functions after
# that source path). The junction is pointed at this checkout for the build and restored afterwards.
# Then it installs into -Destination and adds u59-preset.json, the readme, the license and libFont's
# Roboto fallback fonts. Saves, caches, Kyty.ini and other files already in -Destination are kept.
param(
    [string]$Destination = 'F:\KytyPS5-Build',
    # Where LLVM, CMake, glslang and Qt were unpacked (toolchain\LLVM, toolchain\glslang,
    # build-tools-python\cmake, Qt\6.10.3\msvc2022_64).
    [string]$ToolRoot = 'C:\Users\fahzi\Documents\Codex\2026-10-15\he\work',
    [string]$VsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat',
    [string]$MsvcVersion = '14.51',
    [string]$Ninja = '',
    [switch]$Configure,
    # Other targets (tests) build without installing.
    [string[]]$Targets = @('launcher', 'kyty_emulator'),
    [ValidateRange(0, 64)][int]$Jobs = 0
)
# -File passes "a,b" as one string.
$Targets = @($Targets | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$install = ($Targets -join ',') -eq 'launcher,kyty_emulator'
$ErrorActionPreference = 'Stop'
$repo      = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source    = 'C:\kyty-src19'
$build     = Join-Path $repo '_Build\windows'
$llvm      = Join-Path $ToolRoot 'toolchain\LLVM\bin'
$glslang   = Join-Path $ToolRoot 'toolchain\glslang\bin\glslangValidator.exe'
$cmake     = Join-Path $ToolRoot 'build-tools-python\cmake\data\bin\cmake.exe'
$qt        = Join-Path $ToolRoot 'Qt\6.10.3\msvc2022_64'
$pgoProfile = "$source/tools/pgo/u59-int194-1.profdata"
if (!$Ninja) {
    $found = Get-Command ninja -ErrorAction SilentlyContinue
    if ($found) { $Ninja = $found.Source }
}
foreach ($required in @($VsDevCmd, (Join-Path $llvm 'clang-cl.exe'), (Join-Path $llvm 'lld-link.exe'),
        (Join-Path $llvm 'llvm-lib.exe'), $glslang, $cmake, (Join-Path $qt 'lib\cmake\Qt6\Qt6Config.cmake'), $Ninja)) {
    if (!$required -or !(Test-Path -LiteralPath $required)) { throw "Missing build dependency: $required" }
}
$version = @(& (Join-Path $llvm 'clang-cl.exe') --version)[0]
if ($version -notmatch 'clang version 22\.1\.3\b') { throw "The PGO profile needs LLVM 22.1.3, found: $version" }
if (Get-Process ninja, clang-cl, lld-link -ErrorAction SilentlyContinue) {
    throw 'Another native build is running; it may use the C:\kyty-src19 junction.'
}

# Point C:\kyty-src19 at this checkout for the build; restore the previous target afterwards.
$previous = $null
$changed  = $false
if (Test-Path -LiteralPath $source) {
    $link = Get-Item -LiteralPath $source
    if ($link.LinkType -ne 'Junction') { throw "$source exists and is not a directory junction." }
    $previous = [IO.Path]::GetFullPath([string]@($link.Target)[0])
}
try {
    if ($previous -ne $repo) {
        if ($previous) { [IO.Directory]::Delete($source) }
        New-Item -ItemType Junction -Path $source -Target $repo | Out-Null
        $changed = $true
    }
    $steps = @()
    if ($Configure -or !(Test-Path -LiteralPath (Join-Path $build 'build.ninja'))) {
        $steps += "`"$cmake`" -S $source -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release " +
                  "-DCMAKE_MAKE_PROGRAM=`"$Ninja`" " +
                  "-DCMAKE_C_COMPILER=`"$llvm\clang-cl.exe`" -DCMAKE_CXX_COMPILER=`"$llvm\clang-cl.exe`" " +
                  "-DCMAKE_LINKER=`"$llvm\lld-link.exe`" -DCMAKE_AR=`"$llvm\llvm-lib.exe`" " +
                  "-DCMAKE_PREFIX_PATH=`"$qt`" -DKYTY_GLSLANG_VALIDATOR=`"$glslang`" " +
                  "-DKYTY_EMULATOR_IPO=ON -DKYTY_CP_DIAGNOSTICS=OFF -DKYTY_PGO_USE=$pgoProfile " +
                  "-DKYTY_BUILD_ORIGIN=Fork -DKYTY_BUILD_REPOSITORY=fahziputraj/KytyPS5 -DTRACY_NO_SYSTEM_TRACING=ON"
    }
    $parallel = $(if ($Jobs -gt 0) { "--parallel $Jobs" } else { '--parallel' })
    $steps += "`"$cmake`" --build `"$build`" --target $($Targets -join ' ') $parallel"
    if ($install) { $steps += "`"$cmake`" --install `"$build`" --prefix `"$Destination`"" }
    $script = Join-Path ([IO.Path]::GetTempPath()) ("kyty-build-" + [Guid]::NewGuid().ToString('N') + '.cmd')
    # VsDevCmd.bat fails on some PATH entries with parentheses ("...(x86)\NVIDIA Corporation\...
    # was unexpected"): start it from the system directories, Git and Ninja only.
    $basePath = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
    $git      = Get-Command git -ErrorAction SilentlyContinue
    if ($git) { $basePath += ';' + (Split-Path -Parent $git.Source) }
    $basePath += ';' + (Split-Path -Parent $Ninja)
    $lines  = @('@echo off',
                "set `"PATH=$basePath`"",
                "call `"$VsDevCmd`" -arch=x64 -host_arch=x64 -vcvars_ver=$MsvcVersion >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                "set `"PATH=$llvm;$(Split-Path -Parent $glslang);$qt\bin;%PATH%`"")
    foreach ($step in $steps) { $lines += $step; $lines += 'if errorlevel 1 exit /b %errorlevel%' }
    $lines += 'exit /b 0'
    Set-Content -LiteralPath $script -Value $lines -Encoding ASCII
    try {
        & cmd.exe /d /c $script
        if ($LASTEXITCODE -ne 0) { throw "Build failed: exit code $LASTEXITCODE" }
    } finally {
        Remove-Item -LiteralPath $script -ErrorAction SilentlyContinue
    }
} finally {
    if ($changed) {
        [IO.Directory]::Delete($source)
        if ($previous -and (Test-Path -LiteralPath $previous)) {
            New-Item -ItemType Junction -Path $source -Target $previous | Out-Null
        }
    }
}

if (!$install) { exit 0 }
# What the release archive adds beside the installed files.
Copy-Item -LiteralPath (Join-Path $repo 'tools\u59-preset.json') -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs\EXPERIMENTAL.md') -Destination (Join-Path $Destination 'U59-README.md') -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs\PRELOAD-PACING.md') -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $Destination -Force
$fonts = Join-Path $Destination '3rdparty\tracy\profiler\src\font'
New-Item -ItemType Directory -Path $fonts -Force | Out-Null
Copy-Item -Path (Join-Path $repo '3rdparty\tracy\profiler\src\font\Roboto-*.ttf') -Destination $fonts -Force
Copy-Item -LiteralPath (Join-Path $repo '3rdparty\SPIRV-Tools\LICENSE') -Destination (Join-Path $fonts 'LICENSE-Apache-2.0.txt') -Force
Write-Host "Installed into $Destination"
