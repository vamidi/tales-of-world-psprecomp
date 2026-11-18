param(
    [switch]$SkipTests,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$profile = Join-Path $root 'profiles\tow-1'
$reportPath = Join-Path $profile 'analysis\tow1_report.json'
$binary = Join-Path $root 'out\tow-1\bin\Tow1Native.exe'

$report = Get-Content $reportPath -Raw | ConvertFrom-Json
$units = @(Get-ChildItem (Join-Path $profile 'generated') -Filter 'generated_unit_*.cpp')
$unitBytes = ($units | Measure-Object Length -Sum).Sum
$importWrappers = 0
$registry = Join-Path $profile 'generated\generated_registry.cpp'
if (Test-Path $registry) {
    $importWrappers = @(Select-String -Path $registry -Pattern 'register_function\(').Count
}
$importLibraries = @($report.import_libraries.PSObject.Properties).Count

$logs = @(Get-ChildItem $root -Filter '*.log' |
    Where-Object { $_.Name -match '^(town|guild|battle|field|inn|post_battle)_' })
$missingHle = 0
$missingAot = 0
$geOverflow = 0
foreach ($log in $logs) {
    $missingHle += @(Select-String -Path $log.FullName -Pattern 'Missing HLE|missing HLE|unimplemented' -ErrorAction SilentlyContinue).Count
    $missingAot += @(Select-String -Path $log.FullName -Pattern 'Missing AOT|missing AOT|untranslated' -ErrorAction SilentlyContinue).Count
    $geOverflow += @(Select-String -Path $log.FullName -Pattern 'exceeded the command limit' -ErrorAction SilentlyContinue).Count
}

$testsPassed = $null
if (-not $SkipTests) {
    $testOut = & ctest --test-dir (Join-Path $root 'out\framework') -C Release --output-on-failure 2>&1
    $testsPassed = ($LASTEXITCODE -eq 0)
}

$evidence = [ordered]@{
    'boot/title'          = @('tow1_visible_1.png', 'tow1_visible_3.png')
    'character creation'  = @('field_after_osk.png')
    'field movement'      = @('field_right.png')
    'camera reset'        = @('field_camera2.png')
    'field menu'          = @('field_menu.png')
    'first battle'        = @('battle_first.png')
    'combat damage'       = @('battle_critical.png')
    'battle results'      = @('battle_results.png')
    'battle items/unison' = @('battle_menu_items.png', 'battle_menu_unison.png')
    'post-battle scene'   = @('post_battle_chester.png')
    'town movement/NPC'   = @('town_return.png', 'town_interaction.png')
    'Ailily guild'        = @('town_climb.ppm.32000.png', 'guild_cutscene.ppm.40000.png', 'shop_stairs.ppm.51500.png')
    'guild quest counter' = @()
}
$milestones = foreach ($name in $evidence.Keys) {
    $files = @($evidence[$name])
    $present = $files.Count -gt 0 -and (@($files | Where-Object { Test-Path (Join-Path $root $_) }).Count -eq $files.Count)
    [pscustomobject]@{
        Milestone = $name
        Status = if ($files.Count -eq 0) { 'PENDING' } elseif ($present) { 'VERIFIED' } else { 'EVIDENCE MISSING' }
        Evidence = if ($files.Count -eq 0) { '(no capture yet)' } else { $files -join ', ' }
    }
}

$result = [pscustomobject]@{
    Date = (Get-Date).ToString('yyyy-MM-dd HH:mm')
    Title = "$($report.module.name) / PSP PRX"
    Relocations = "{0:N0} (invalid {1}, unsupported {2})" -f $report.relocations.total, $report.relocations.invalid, $report.relocations.unsupported
    ImportLibraries = $importLibraries
    Imports = $report.import_count
    ImportWrappers = $importWrappers
    FunctionSeeds = $report.function_seed_count
    TruncatedFunctions = $report.automatic_analysis.truncated_functions
    GeneratedUnits = $units.Count
    GeneratedMB = [math]::Round($unitBytes / 1MB, 2)
    BinaryMB = if (Test-Path $binary) { [math]::Round((Get-Item $binary).Length / 1MB, 1) } else { 0 }
    DiagnosticLogs = $logs.Count
    MissingHle = $missingHle
    MissingAot = $missingAot
    GeCommandLimit = $geOverflow
    FrameworkTests = if ($null -eq $testsPassed) { 'skipped' } elseif ($testsPassed) { '1/1 passed' } else { 'FAILED' }
    VerifiedMilestones = @($milestones | Where-Object Status -eq 'VERIFIED').Count
    TotalMilestones = @($milestones).Count
    Milestones = @($milestones)
}

if ($Json) {
    $result | ConvertTo-Json -Depth 4
    exit 0
}

Write-Host ''
Write-Host 'Tow1 port status' -ForegroundColor Cyan
Write-Host ("  {0}" -f $result.Date)
Write-Host ''
Write-Host 'Execution and corpus' -ForegroundColor Cyan
Write-Host ("  Relocations:        {0}" -f $result.Relocations)
Write-Host ("  Imports:            {0} across {1} libraries ({2} wrappers)" -f $result.Imports, $result.ImportLibraries, $result.ImportWrappers)
Write-Host ("  AOT seeds:          {0:N0} ({1} truncated)" -f $result.FunctionSeeds, $result.TruncatedFunctions)
Write-Host ("  Generated corpus:   {0} units, {1} MB" -f $result.GeneratedUnits, $result.GeneratedMB)
Write-Host ("  Binary:             {0} MB" -f $result.BinaryMB)
Write-Host ''
Write-Host 'Runtime health' -ForegroundColor Cyan
Write-Host ("  Diagnostic logs:    {0}" -f $result.DiagnosticLogs)
Write-Host ("  Missing HLE:        {0}" -f $result.MissingHle)
Write-Host ("  Missing AOT:        {0}" -f $result.MissingAot)
Write-Host ("  GE command limits:  {0}" -f $result.GeCommandLimit)
Write-Host ("  Framework tests:    {0}" -f $result.FrameworkTests)
Write-Host ''
Write-Host ("Verified milestones  {0}/{1}" -f $result.VerifiedMilestones, $result.TotalMilestones) -ForegroundColor Cyan
$milestones | Format-Table -AutoSize
Write-Host ''
Write-Host 'Interpretation: this is a milestone/evidence scorecard, not a single percentage.' -ForegroundColor DarkGray
Write-Host 'Missing HLE/AOT/GE counts are from the diagnostic logs present in the workspace.' -ForegroundColor DarkGray
