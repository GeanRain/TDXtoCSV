param([switch]$BuildDll, [string]$OutputRoot = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $PSScriptRoot
$OutputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputRoot)
$runDir = Join-Path $OutputRoot ('TDXtoCVS-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsDir = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsDir) { throw '未发现 VS2022 C++ 工具链。' }
$vcDir = Get-ChildItem -LiteralPath (Join-Path $vsDir 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkDir = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem -LiteralPath (Join-Path $sdkDir 'Include') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $vcDir -or -not $sdkVersion) { throw '未发现 MSVC v143 或 Windows SDK。' }
$env:INCLUDE = @((Join-Path $vcDir.FullName 'include'), (Join-Path $sdkVersion.FullName 'ucrt'), (Join-Path $sdkVersion.FullName 'shared'), (Join-Path $sdkVersion.FullName 'um'), (Join-Path $sdkVersion.FullName 'winrt')) -join ';'
$env:LIB = @((Join-Path $vcDir.FullName 'lib\x86'), (Join-Path $sdkDir ('Lib\' + $sdkVersion.Name + '\ucrt\x86')), (Join-Path $sdkDir ('Lib\' + $sdkVersion.Name + '\um\x86'))) -join ';'
$compilerDir = Join-Path $vcDir.FullName 'bin\Hostx64\x86'
$env:PATH = $compilerDir + ';' + $env:PATH
$compiler = Join-Path $compilerDir 'cl.exe'
$asyncSource = Join-Path $projectDir 'SaveAsCSV\export\async_writer.cpp'
Write-Host "测试输出与样例：$runDir"
Push-Location -LiteralPath $runDir
try {
    $testExe = Join-Path $runDir 'export_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /W4 /DWIN32 /DNOMINMAX (Join-Path $PSScriptRoot 'export_tests.cpp') $asyncSource (Join-Path $projectDir 'SaveAsCSV\utils\ini_reader.cpp') "/Fe:$testExe" /link /DEBUG:NONE /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $testExe $runDir
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { exit $testExit }
    $asyncExe = Join-Path $runDir 'async_writer_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /W4 /DWIN32 /DNOMINMAX (Join-Path $PSScriptRoot 'async_writer_tests.cpp') "/Fe:$asyncExe" /link /DEBUG:NONE /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $asyncExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $loggerExe = Join-Path $runDir 'logger_memory_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /W4 /DWIN32 /DNOMINMAX (Join-Path $PSScriptRoot 'logger_memory_tests.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\logger.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\ini_reader.cpp') "/Fe:$loggerExe" /link /DEBUG:NONE /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $loggerExe $runDir
    if ($LASTEXITCODE -ne 0 -or -not $BuildDll) { exit $LASTEXITCODE }
    $conversionExe = Join-Path $runDir 'csv_conversion_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /W4 /DWIN32 /DNOMINMAX (Join-Path $PSScriptRoot 'csv_conversion_tests.cpp') "/Fe:$conversionExe" /link /DEBUG:NONE /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $conversionExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $workerPath = Join-Path $runDir 'SaveAsCSVWorker.exe'
    $ipcSource = Join-Path $projectDir 'shared\ipc.cpp'
    $storeSource = Join-Path $projectDir 'shared\csv_store.cpp'
    $processSource = Join-Path $projectDir 'SaveAsCSV\export\process_writer.cpp'
    $ipcExe = Join-Path $runDir 'ipc_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /W4 /DWIN32 /DNOMINMAX (Join-Path $PSScriptRoot 'ipc_tests.cpp') $ipcSource $storeSource "/Fe:$ipcExe" /link /DEBUG:NONE /INCREMENTAL:NO advapi32.lib
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $ipcExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MT /O2 /W4 /DWIN32 /DNOMINMAX (Join-Path $projectDir 'SaveAsCSVWorker\main.cpp') $ipcSource $storeSource $asyncSource (Join-Path $projectDir 'SaveAsCSV\utils\ini_reader.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\logger.cpp') "/Fe:$workerPath" /link /SUBSYSTEM:WINDOWS /LARGEADDRESSAWARE /DEBUG:NONE /INCREMENTAL:NO advapi32.lib shell32.lib
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $testWorkerPath = Join-Path $runDir 'TestWorker.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MT /O2 /W4 /DWIN32 /DNOMINMAX /DTDX_WORKER_TEST (Join-Path $projectDir 'SaveAsCSVWorker\main.cpp') (Join-Path $PSScriptRoot 'worker_test_hooks.cpp') $ipcSource $asyncSource (Join-Path $projectDir 'SaveAsCSV\utils\ini_reader.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\logger.cpp') "/Fe:$testWorkerPath" /link /SUBSYSTEM:WINDOWS /LARGEADDRESSAWARE /DEBUG:NONE /INCREMENTAL:NO advapi32.lib shell32.lib
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $processExe = Join-Path $runDir 'process_writer_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /W4 /DWIN32 /DNOMINMAX (Join-Path $PSScriptRoot 'process_writer_tests.cpp') $processSource $ipcSource $storeSource (Join-Path $projectDir 'SaveAsCSV\utils\ini_reader.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\logger.cpp') "/Fe:$processExe" /link /DEBUG:NONE /INCREMENTAL:NO advapi32.lib psapi.lib
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $processExe $workerPath $testWorkerPath (Join-Path $runDir 'process')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $dllPath = Join-Path $runDir 'SaveAsCSV.dll'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MT /O2 /LD /DWIN32 /DNDEBUG /DNOMINMAX $storeSource $ipcSource $processSource (Join-Path $projectDir 'SaveAsCSV\dllmain.cpp') (Join-Path $projectDir 'SaveAsCSV\link\tdx\linkTDX.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\ini_reader.cpp') (Join-Path $projectDir 'SaveAsCSV\utils\logger.cpp') "/Fe:$dllPath" /link /DEBUG:NONE /INCREMENTAL:NO advapi32.lib "/IMPLIB:$(Join-Path $runDir 'SaveAsCSV.lib')"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $testExe $runDir $dllPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $exitExe = Join-Path $runDir 'worker_exit_tests.exe'
    & $compiler /nologo /std:c++17 /EHsc /utf-8 /MD /O2 /W4 /DNOMINMAX (Join-Path $PSScriptRoot 'worker_exit_tests.cpp') "/Fe:$exitExe" /link /DEBUG:NONE /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $exitExe $dllPath $testWorkerPath (Join-Path $runDir 'host-exit')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $releaseDll = Join-Path $projectDir 'Release\SaveAsCSV.dll'
    New-Item -ItemType Directory -Path (Split-Path -Parent $releaseDll) -Force | Out-Null
    Copy-Item -LiteralPath $dllPath -Destination $releaseDll
    Copy-Item -LiteralPath $workerPath -Destination (Join-Path $projectDir 'Release\SaveAsCSVWorker.exe')
    Write-Host "已验证并构建 Win32 DLL 和独立后台：$releaseDll"
    exit 0
}
finally { Pop-Location }
