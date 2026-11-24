# Replays the locally retained v1.0.0 fixtures against a Windows build.
param(
    [string]$FixtureRoot = '.data/compat-v1.0.0/fixtures',
    [string]$DevelopmentExe = 'build/Release/smsgg_win32.exe'
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Resolve-Path "$PSScriptRoot/../..").Path
$FixtureRoot = (Resolve-Path -LiteralPath $FixtureRoot).Path
$cases = @(
    @{ Name = 'sms'; Directory = '.'; Rom = 'compat.sms'; Machine = 0 }
    @{ Name = 'gg'; Directory = 'gg'; Rom = 'compat.gg'; Machine = 1 }
    @{ Name = 'sg1000'; Directory = 'sg1000'; Rom = 'compat.sg'; Machine = 2 }
)
foreach ($backend in 'emu2413', 'ymfm') {
    foreach ($method in 'sinc', 'zoh-low-pass', 'linear') {
        $directory = "fm-$backend"
        if ($method -ne 'sinc') { $directory += "-$method" }
        $cases += @{ Name = "fm-$backend-$method"; Directory = $directory; Rom = 'fm-tone.sms'
            Backend = $backend; Method = $method }
    }
}

function Confirm-Hash($path, $expected) {
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) {
        throw "SHA-256 mismatch: $path"
    }
}
foreach ($case in $cases) {
    $case.Source = Join-Path $FixtureRoot $case.Directory
    $expectedFiles = @('acesms.sqlite3', $case.Rom)
    if (!$case.Backend) { $expectedFiles += 'cheats-v1.json' }
    $manifest = Join-Path $case.Source 'SHA256SUMS.txt'
    $case.Hashes = @{ 'SHA256SUMS.txt' = (Get-FileHash -LiteralPath $manifest).Hash }
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "Invalid manifest entry: $manifest" }
        $digest, $name = $Matches[1], $Matches[2]
        if ($name -cnotin $expectedFiles -or $case.Hashes.ContainsKey($name)) {
            throw "Unexpected manifest entry: $manifest ($name)"
        }
        $case.Hashes[$name] = $digest
        Confirm-Hash (Join-Path $case.Source $name) $digest
    }
    if ($case.Hashes.Count -ne $expectedFiles.Count + 1) { throw "Missing fixture entry: $manifest" }
    "Verified originals: $($case.Name)"
}

$DevelopmentExe = (Resolve-Path -LiteralPath $DevelopmentExe).Path
$pluginPath = 'plugins/fm/acesms-fm-ymfm.dll'
$developmentPlugin = (Resolve-Path -LiteralPath (Join-Path (Split-Path -Parent $DevelopmentExe) $pluginPath)).Path
$run = (New-Item -ItemType Directory -Path (Join-Path '.data/compat-v1.0.0' ('replay-' + [guid]::NewGuid().ToString('N')))).FullName
"Run directory: $run"
"Development executable SHA-256: $((Get-FileHash -LiteralPath $DevelopmentExe).Hash)"
"Development ymfm DLL SHA-256: $((Get-FileHash -LiteralPath $developmentPlugin).Hash)"

Add-Type -Path "$PSScriptRoot/../win32/NativeWindowProbe.cs"
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CompatibilityControls {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr value, IntPtr data);
    [DllImport("user32.dll")] static extern IntPtr GetMenu(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetSubMenu(IntPtr menu, int position);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr menu, int position);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern int GetMenuStringW(IntPtr menu, uint item, StringBuilder text, int size, uint flags);
    public static string StateSlotText(IntPtr window, int slot) {
        var file = GetSubMenu(GetMenu(window), 0);
        for (var i = 0; i < GetMenuItemCount(file); ++i) {
            var submenu = GetSubMenu(file, i);
            if (submenu == IntPtr.Zero || GetMenuItemID(submenu, 0) != 1360) continue;
            var text = new StringBuilder(512);
            GetMenuStringW(submenu, (uint)(slot - 1), text, text.Capacity, 0x400);
            return text.ToString();
        }
        return "";
    }
}
'@

function Wait-For([scriptblock]$condition) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if (& $condition) { return }
        if ($app.HasExited) { throw "Application exited early ($($app.ExitCode))" }
        Start-Sleep -Milliseconds 40
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "UI timeout: $([Stage10WindowProbe]::Title($main)) | $([Stage10WindowProbe]::Dialogs($app.Id) -join ' / ')"
}
function Post([IntPtr]$window, [uint32]$message, [int]$value = 0) {
    if (![Stage10WindowProbe]::PostMessageW($window, $message, [IntPtr]$value, [IntPtr]::Zero)) {
        throw "PostMessage failed: $message / $value"
    }
}
function Send([IntPtr]$window, [uint32]$message, [int]$value = 0, [IntPtr]$data = [IntPtr]::Zero) {
    if ($window -eq [IntPtr]::Zero) { throw "Window missing for message $message" }
    [CompatibilityControls]::SendMessageW($window, $message, [IntPtr]$value, $data).ToInt64()
}
function Status([string]$pattern) {
    Wait-For { [Stage10WindowProbe]::Title($main) -match $pattern }
}
function Modal([string]$title) {
    Wait-For {
        $script:dialog = [Stage10WindowProbe]::Find($app.Id, '#32770', $main)
        [Stage10WindowProbe]::IsWindowVisible($dialog) -and [Stage10WindowProbe]::Title($dialog) -eq $title
    }
}
function No-Modal {
    Wait-For { [Stage10WindowProbe]::Find($app.Id, '#32770', $main) -eq [IntPtr]::Zero }
}
function Tap-Key([int]$key) {
    Post $main 0x100 $key
    Post $main 0x101 $key
}
function Select-Choice([int]$id, [int]$index) {
    $combo = [Stage10WindowProbe]::Child($dialog, 'ComboBox', $id)
    if ((Send $combo 0x14e $index) -ne $index) { throw "Cannot select combo $id item $index" }
    $null = Send $dialog 0x111 ($id -bor 0x10000) $combo
}
function Pick-File([IntPtr]$owner, [string]$path) {
    Wait-For {
        $script:picker = [Stage10WindowProbe]::Find($app.Id, '#32770', $owner)
        if (![Stage10WindowProbe]::IsWindowVisible($picker)) { return $false }
        $script:filenameEdit = [Stage10WindowProbe]::Child($picker, 'Edit', 1148)
        if ($filenameEdit -eq [IntPtr]::Zero) { $script:filenameEdit = [Stage10WindowProbe]::Child($picker, 'Edit', 1001) }
        $filenameEdit -ne [IntPtr]::Zero
    }
    [Stage10WindowProbe]::SetText($filenameEdit, $path)
    Post ([Stage10WindowProbe]::Child($picker, 'Button', 1)) 0xf5
    Wait-For { ![Stage10WindowProbe]::IsWindowVisible($picker) }
}
function Start-App([string]$folder) {
    $script:app = Start-Process -FilePath (Join-Path $folder 'acesms.exe') -WorkingDirectory $folder -WindowStyle Hidden -PassThru
    $script:main = [IntPtr]::Zero
    $null = $app.WaitForInputIdle(10000)
    Wait-For {
        $script:main = [Stage10WindowProbe]::Find($app.Id, 'SmsGgStage5Window', [IntPtr]::Zero)
        $main -ne [IntPtr]::Zero
    }
    $null = [Stage10WindowProbe]::ShowWindow($main, 4)
    Post $main 0x111 1100 # Host pause, including before opening the ROM.
}
function Close-App {
    Post $main 0x10
    if (!$app.WaitForExit(15000) -or $app.ExitCode -ne 0) { throw 'Application failed to exit normally' }
}
function Open-Rom($case, $rom, [switch]$Register) {
    Post $main 0x111 1001
    Pick-File $main $rom
    if ($Register) {
        Modal 'Register ROM'
        Select-Choice 203 $case.Machine
        if ($case.Machine -ne 2) {
            Select-Choice 205 1
            Select-Choice 206 3
            $null = Send ([Stage10WindowProbe]::Child($dialog, 'Button', 207)) 0xf1
        }
        Post $dialog 0x111 1
    }
    Status "$([regex]::Escape($case.Rom)).*Paused.*ROM loaded"
    No-Modal
}
function Open-Cheats {
    Wait-For { [Stage10WindowProbe]::MenuEnabled($main, 1105) }
    Post $main 0x111 1105 # Cheat Editor
    Modal 'Memory Search and Cheats'
    if ((Send ([Stage10WindowProbe]::Child($dialog, 'SysTabControl32', 300)) 0x130b) -ne 1) { throw 'Cheats tab missing' }
}
function Cheat-Count {
    Send ([Stage10WindowProbe]::Child($dialog, 'ListBox', 309)) 0x18b
}
function Close-Cheats {
    Post $dialog 0x111 2 # Resume
    No-Modal
}
function Save-Slot([int]$slot) {
    Tap-Key (48 + $slot)
    Tap-Key 116
    Wait-For {
        if ([Stage10WindowProbe]::Title($main) -notmatch 'State saved') { return $false }
        # The menu identifies the new slot without opening the live database during F5.
        $label = [CompatibilityControls]::StateSlotText($main, $slot)
        $label -and !$label.Contains('(Empty)')
    }
}
function Initialize-App($folder, $case) {
    $null = New-Item -ItemType Directory -Path $folder
    Copy-Item -LiteralPath $DevelopmentExe -Destination (Join-Path $folder 'acesms.exe')
    if ($case.Backend -eq 'ymfm') {
        $target = Join-Path $folder $pluginPath
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $target)
        Copy-Item -LiteralPath $developmentPlugin -Destination $target
    }
}
function Inspect($case, $phase, $db, $baseline, $rom, $json) {
    if ($phase -ne 'before-save') {
        foreach ($suffix in '-journal', '-wal', '-shm') {
            if (Test-Path -LiteralPath "$db$suffix") { throw "SQLite sidecar after exit: $db$suffix" }
        }
    }
    node --no-warnings "$PSScriptRoot/Inspect-V1Compatibility.cjs" $case.Name $phase $db $baseline $rom $json
    if ($LASTEXITCODE -ne 0) { throw "SQLite inspection failed: $($case.Name) / $phase" }
}

function Invoke-Case($case, $directory) {
    $inputCopy = (New-Item -ItemType Directory -Path (Join-Path $directory 'input')).FullName
    foreach ($name in $case.Hashes.Keys) {
        $copy = Join-Path $inputCopy $name
        Copy-Item -LiteralPath (Join-Path $case.Source $name) -Destination $copy
        Confirm-Hash $copy $case.Hashes[$name]
    }
    $baseline = Join-Path $inputCopy 'acesms.sqlite3'
    $rom = Join-Path $inputCopy $case.Rom
    $json = Join-Path $inputCopy 'cheats-v1.json'
    $reuse = Join-Path $directory 'reuse'
    Initialize-App $reuse $case
    $db = Join-Path $reuse 'acesms.sqlite3'
    Copy-Item -LiteralPath $baseline -Destination $db
    Start-App $reuse
    if (!$case.Backend) {
        Post $main 0x111 1008
        Modal 'Settings'
        if ((Send ([Stage10WindowProbe]::Child($dialog, 'ComboBox', 206)) 0x147) -ne 2) { throw 'Saved default RAM was not displayed' }
        Post $dialog 0x111 2
        No-Modal
    }
    Open-Rom $case $rom
    if (!$case.Backend) {
        Open-Cheats
        if ((Cheat-Count) -ne 1) { throw 'Saved cheat missing in editor' }
        Close-Cheats
        Save-Slot 6
    }
    Tap-Key 53
    Tap-Key 117
    Status 'State loaded'
    if ($case.Backend) {
        $fmName = if ($case.Backend -eq 'ymfm') { 'ymfm OPLL' } else { 'emu2413 1.6.4' }
        $interpolation = @{ sinc = 'Sinc'; 'zoh-low-pass' = 'Zero-order hold + low-pass filter'; linear = 'Linear' }[$case.Method]
        Post $main 0x111 1301
        Modal 'ROM information'
        $info = [Stage10WindowProbe]::DialogText($dialog)
        if (!$info.Contains("FM: $fmName") -or !$info.Contains("Interpolation: $interpolation (version 1)")) { throw "Restored FM profile mismatch: $info" }
        Post $dialog 0x10
        No-Modal
    }
    Save-Slot 7
    if ($case.Backend) {
        Post $main 0x111 1100
        Status 'Running'
        Start-Sleep -Milliseconds 500
        Post $main 0x111 1100
        Status 'Paused'
        Save-Slot 8
    }
    Close-App
    Inspect $case 'reuse' $db $baseline $rom $json

    if (!$case.Backend) {
        $import = Join-Path $directory 'import'
        Initialize-App $import $case
        $db = Join-Path $import 'acesms.sqlite3'
        Start-App $import
        Open-Rom $case $rom -Register
        Open-Cheats
        Post $dialog 0x111 326
        Pick-File $dialog $json
        Wait-For { (Cheat-Count) -eq 1 -and [Stage10WindowProbe]::DialogText($dialog).Contains('1 imported (disabled)') }
        Inspect $case 'before-save' $db $baseline $rom $json
        Post $dialog 0x111 324
        Wait-For { [Stage10WindowProbe]::DialogText($dialog).Contains('Cheats saved.') }
        Close-Cheats
        Close-App
        Start-App $import
        Open-Rom $case $rom
        Open-Cheats
        if ((Cheat-Count) -ne 1) { throw 'Imported cheat missing after restart' }
        Close-Cheats
        Close-App
        Inspect $case 'imported' $db $baseline $rom $json
    }
    "PASS $($case.Name)"
}

try {
    foreach ($case in $cases) {
        $directory = (New-Item -ItemType Directory -Path (Join-Path $run $case.Name)).FullName
        Invoke-Case $case $directory *>&1 | Tee-Object -FilePath (Join-Path $directory 'acceptance.log')
    }
} finally {
    if ($script:app -and !$app.HasExited) { Stop-Process -Id $app.Id }
    foreach ($case in $cases) {
        foreach ($name in $case.Hashes.Keys) { Confirm-Hash (Join-Path $case.Source $name) $case.Hashes[$name] }
    }
    'Original hashes rechecked'
}
"PASS $($cases.Count)/9 configurations"
