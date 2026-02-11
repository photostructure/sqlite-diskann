# Build and test sqlite-diskann on Windows with MSVC
# Usage: powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1 [test|extension]
#
# Requires: Visual Studio Build Tools (cl.exe on PATH via vcvarsall.bat or Developer Shell)

param(
    [ValidateSet("test", "extension")]
    [string]$Target = "test"
)

$ErrorActionPreference = "Stop"

# Directories
$BuildDir = "build"
$SrcDir = "src"
$TestDir = "tests"
$VendorSqlite = "vendor/sqlite"

# Source files
$Sources = @(
    "$SrcDir/diskann_api.c",
    "$SrcDir/diskann_blob.c",
    "$SrcDir/diskann_insert.c",
    "$SrcDir/diskann_node.c",
    "$SrcDir/diskann_search.c",
    "$SrcDir/diskann_vtab.c"
)

$TestSources = Get-ChildItem "$TestDir/c/test_*.c" | Where-Object { $_.Name -ne "test_runner.c" -and $_.Name -ne "test_stress.c" } | ForEach-Object { $_.FullName }
$TestRunner = "$TestDir/c/test_runner.c"
$UnitySrc = "$TestDir/c/unity/unity.c"
$SqliteSrc = "$VendorSqlite/sqlite3.c"

# Create build directory
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Common MSVC flags
# /std:c17   - C17 standard
# /W4        - High warning level (MSVC equivalent of -Wall -Wextra)
# /WX        - Treat warnings as errors
# /wd4702    - Disable "unreachable code" warning (Unity TEST_FAIL uses longjmp)
# /O2        - Optimize for speed
# /nologo    - Suppress banner
$CommonFlags = @("/std:c17", "/W4", "/WX", "/wd4702", "/O2", "/nologo", "/I$VendorSqlite")

# Build vendored SQLite (relaxed warnings - SQLite triggers MSVC warnings)
Write-Host "Building vendored SQLite 3.51.2..."
cl.exe /std:c17 /O2 /nologo /W0 /DSQLITE_OMIT_LOAD_EXTENSION /c /Fo"$BuildDir/sqlite3.obj" $SqliteSrc
if ($LASTEXITCODE -ne 0) { throw "SQLite compilation failed" }

if ($Target -eq "extension") {
    # Build shared library (DLL) with vendored SQLite linked in.
    # SQLite extensions on Windows must resolve SQLite symbols at link time.
    Write-Host "Building diskann.dll..."
    cl.exe @CommonFlags /LD /Fe"$BuildDir/diskann.dll" @Sources "$BuildDir/sqlite3.obj"
    if ($LASTEXITCODE -ne 0) { throw "Extension build failed" }
    Write-Host "Built: $BuildDir/diskann.dll"
}
elseif ($Target -eq "test") {
    # Build and run test executable
    Write-Host "Building test suite..."
    $AllSources = $Sources + $TestSources + @($TestRunner, $UnitySrc, "$BuildDir/sqlite3.obj")
    cl.exe @CommonFlags /I"$SrcDir" /I"$TestDir/c" /Fe"$BuildDir/test_diskann.exe" @AllSources
    if ($LASTEXITCODE -ne 0) { throw "Test build failed" }
    Write-Host "Built test suite: $BuildDir/test_diskann.exe"

    Write-Host "Running tests..."
    & "$BuildDir/test_diskann.exe"
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}
