#Requires -Version 5.1
<#
.SYNOPSIS
Install atari-os-x86 next to Windows (dual boot, UEFI machines).

.DESCRIPTION
Copies the files from atari-os-x86-<version>-files.zip onto a FAT32
partition and adds ONE entry, "Atari", at the END of the firmware's boot
list, so it shows in the one-time boot menu (F12, F9, Esc ... at power-on).
Windows stays the default: the boot order's first entry, the Windows Boot
Manager's default and its timeout are not changed (except with -Menu, see
below). Secure Boot must be off (the Atari's boot loader is unsigned).

The FAT32 partition is either an existing, empty FAT32 volume (-Partition)
or a new one made by shrinking C: (-ShrinkMB). Existing volumes are never
formatted. Every change is shown first and needs "yes" typed.

Run it from an administrator PowerShell:
  powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Zip atari-os-x86-<v>-files.zip -ShrinkMB 2048
-Check and -WhatIf also work without administrator rights (they show less).

The script writes X:\atari-os\windows-install.json on the Atari partition:
the identifiers of the entries it made and, with -Menu, the Boot Manager
timeout it replaced, so -Remove undoes exactly that.

Tested on Windows 11 by: nobody yet.

.PARAMETER Zip
The files zip from the release (atari-os-x86-<version>-files.zip).

.PARAMETER Partition
An existing FAT32 volume of 1.5 GB or more, empty (or an earlier Atari
install), e.g. E: (format it as FAT32 in Disk Management first).

.PARAMETER ShrinkMB
Make room by shrinking the Windows partition (C:) by this many MB
(1536 to 32768) and create a new FAT32 partition "ATARI" there.

.PARAMETER Remove
Delete the firmware entry (and the -Menu entry, restoring the Boot
Manager timeout). The partition and its files stay.

.PARAMETER Check
Show what the script sees (firmware, Secure Boot, BitLocker, partitions,
existing Atari entries); change nothing. The default without parameters.

.PARAMETER BootNow
After installing: start the Atari once at the next restart (UEFI
BootNext, the firmware's default stays Windows) and restart now (asks).

.PARAMETER Menu
Also add "Atari" to the Windows Boot Manager menu, for firmware that hides
extra boot entries. The blue Windows chooser then shows at every start
(timeout set to 10 s if it was 0 or the menu was never shown; -Remove puts
it back). Some Windows versions refuse to start a non-Windows loader from
there; the firmware boot menu always works.

.PARAMETER Name
The title of the entries (default "Atari").

.EXAMPLE
.\install-windows.ps1 -Check

.EXAMPLE
.\install-windows.ps1 -Zip .\atari-os-x86-v0.2.0-files.zip -ShrinkMB 2048 -WhatIf

.EXAMPLE
.\install-windows.ps1 -Zip .\atari-os-x86-v0.2.0-files.zip -Partition E: -BootNow

.EXAMPLE
.\install-windows.ps1 -Remove

.NOTES
PowerShell 5.1 (Windows 10 and 11). Licence: GPL-2.0-or-later, like the
rest of atari-os-x86.
#>
[CmdletBinding(SupportsShouldProcess = $true, DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Partition', Mandatory = $true)]
    [Parameter(ParameterSetName = 'Shrink', Mandatory = $true)]
    [string]$Zip,

    [Parameter(ParameterSetName = 'Partition', Mandatory = $true)]
    [string]$Partition,

    [Parameter(ParameterSetName = 'Shrink', Mandatory = $true)]
    [ValidateRange(1536, 32768)]
    [int]$ShrinkMB,

    [Parameter(ParameterSetName = 'Remove', Mandatory = $true)]
    [switch]$Remove,

    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,

    [Parameter(ParameterSetName = 'Partition')]
    [Parameter(ParameterSetName = 'Shrink')]
    [switch]$BootNow,

    [Parameter(ParameterSetName = 'Partition')]
    [Parameter(ParameterSetName = 'Shrink')]
    [switch]$Menu,

    [ValidatePattern('^[A-Za-z0-9 ._+()-]{1,40}$')]
    [string]$Name = 'Atari'
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$LoaderPath = '\EFI\BOOT\BOOTX64.EFI'
$Sidecar = 'atari-os\windows-install.json'
$MinBytes = [uint64]1536 * [uint64]1048576
$GuidRe = '\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}'
$BasicDataGpt = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$EspGpt = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$RecoveryGpt = '{de94bba4-06d1-4d40-a16a-bfd50179d6ac}'
$MsrGpt = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
$OurTopLevel = @('atari-os', 'atari', 'EFI', 'System Volume Information')

# ---- output -----------------------------------------------------------------

function Say([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Note([string]$Text) { Write-Host $Text }
function Warn([string]$Text) { Write-Host "warning: $Text" -ForegroundColor Yellow }
function Fail([string]$Text) {
    Write-Host "error: $Text" -ForegroundColor Red
    exit 1
}

# ---- environment ------------------------------------------------------------

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# 'UEFI', 'BIOS' or 'unknown'
function Get-FirmwareType {
    # Windows sets firmware_type to "UEFI" or "Legacy"
    if ($env:firmware_type -eq 'UEFI') { return 'UEFI' }
    if ($env:firmware_type -eq 'Legacy') { return 'BIOS' }
    try {
        $v = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name PEFirmwareType).PEFirmwareType
        if ($v -eq 2) { return 'UEFI' }
        if ($v -eq 1) { return 'BIOS' }
    } catch { }
    return 'unknown'
}

# 'on', 'off' or 'unknown' (the cmdlet needs administrator rights and throws
# on machines without UEFI or without Secure Boot support)
function Get-SecureBootState {
    try {
        if (Confirm-SecureBootUEFI) { return 'on' } else { return 'off' }
    } catch {
        return 'unknown'
    }
}

function Get-BitLockerState([string]$Mount) {
    try {
        $b = Get-BitLockerVolume -MountPoint $Mount
        return "$($b.VolumeStatus), protection $($b.ProtectionStatus)"
    } catch {
        return 'unknown (needs administrator rights)'
    }
}

# Fast Startup: Windows "shuts down" by hibernating its kernel with the
# volumes still mounted.
function Test-FastStartup {
    try {
        $v = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled).HiberbootEnabled
        return ($v -eq 1)
    } catch {
        return $false
    }
}

function Get-SystemLetter { return $env:SystemDrive.Substring(0, 1).ToUpper() }

# ---- bcdedit ----------------------------------------------------------------

# Run bcdedit; its exit code decides (its messages are localised), and on
# failure its output is reported verbatim.
function Invoke-Bcd([string[]]$BcdArgs) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # 5.1 turns native stderr into errors
    try {
        $out = (& bcdedit.exe @BcdArgs 2>&1 | Out-String)
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    if ($code -ne 0) {
        throw "bcdedit $($BcdArgs -join ' ') failed (exit $code): $($out.Trim())"
    }
    return $out
}

function Get-GuidFrom([string]$Text) {
    $m = [regex]::Match($Text, $GuidRe)
    if (-not $m.Success) { throw "no {GUID} in bcdedit's answer: $($Text.Trim())" }
    return $m.Value.ToLower()
}

function Test-BcdEntry([string]$Guid) {
    if (-not $Guid) { return $false }
    try { Invoke-Bcd @('/enum', $Guid) | Out-Null; return $true } catch { return $false }
}

# Entries of a bcdedit /enum listing as @{ Guid; Text } (blocks between blank
# lines; the identifier is the block's first {GUID}).
function Get-BcdBlocks([string]$Listing) {
    $blocks = @()
    foreach ($b in ($Listing -split '(?:\r?\n){2,}')) {
        $m = [regex]::Match($b, $GuidRe)
        if ($m.Success) { $blocks += New-Object PSObject -Property @{ Guid = $m.Value.ToLower(); Text = $b } }
    }
    return $blocks
}

# Our entries without the sidecar: description = Name and path = the loader.
function Find-OurEntries([string]$Kind) {
    $found = @()
    try { $listing = Invoke-Bcd @('/enum', $Kind) } catch { return $found }
    foreach ($b in (Get-BcdBlocks $listing)) {
        $lines = $b.Text -split "\r?\n"
        $hasPath = $false; $hasName = $false
        foreach ($l in $lines) {
            if ($l -match '\s(\\EFI\\BOOT\\BOOTX64\.EFI)\s*$') { $hasPath = $true }
            if ($l -match ('\s' + [regex]::Escape($Name) + '\s*$')) { $hasName = $true }
        }
        if ($hasPath -and $hasName) { $found += $b.Guid }
    }
    return $found
}

# The Windows Boot Manager's timeout (seconds, $null when unknown) and the
# number of entries in its displayorder (the menu only shows with 2 or more;
# 0 when unknown).
function Get-BootMgrMenu {
    try { $t = Invoke-Bcd @('/enum', '{bootmgr}') } catch { return $null }
    $timeout = $null
    $m = [regex]::Match($t, '(?m)^timeout\s+(\d+)\s*$')
    if ($m.Success) { $timeout = [int]$m.Groups[1].Value }
    # "displayorder  {current}" then one indented line per further entry
    $entries = 0
    $in = $false
    foreach ($l in ($t -split '\r?\n')) {
        if ($l -match '^displayorder\s+\{') { $in = $true; $entries = 1; continue }
        if ($in) {
            if ($l -match '^\s+\{') { $entries++ } else { $in = $false }
        }
    }
    return New-Object PSObject -Property @{ Timeout = $timeout; Entries = $entries }
}

# ---- the zip ----------------------------------------------------------------

function Test-Zip([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail "no such file: $Path" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = $null
    try {
        $z = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Path).ProviderPath)
        $names = @{}
        [uint64]$total = 0
        foreach ($e in $z.Entries) {
            $names[$e.FullName.Replace('\', '/').ToLower()] = $true
            $total += [uint64]$e.Length
        }
    } catch {
        Fail "$Path is not a readable zip: $($_.Exception.Message)"
    } finally {
        if ($z) { $z.Dispose() }
    }
    foreach ($need in @('efi/boot/bootx64.efi', 'atari-os/vmlinuz', 'atari-os/initramfs', 'limine.conf')) {
        if (-not $names.ContainsKey($need)) { Fail "$Path has no $need`: not an atari-os-x86 files zip" }
    }
    return $total
}

# ---- partitions -------------------------------------------------------------

function Get-PartitionFlags($P) {
    $flags = @()
    if ($P.IsSystem) { $flags += 'EFI system partition' }
    if ($P.IsBoot) { $flags += 'the Windows partition' }
    $gpt = ''
    if ($P.GptType) { $gpt = ([string]$P.GptType).ToLower() }
    if ($gpt -eq $EspGpt -and -not $P.IsSystem) { $flags += 'EFI system partition' }
    if ($gpt -eq $RecoveryGpt -or [string]$P.Type -eq 'Recovery') { $flags += 'recovery partition' }
    if ($gpt -eq $MsrGpt -or [string]$P.Type -eq 'Reserved') { $flags += 'Microsoft reserved partition' }
    return $flags
}

# Top-level names on X: that are not ours ('' when none)
function Get-ForeignFiles([string]$Letter) {
    $bad = @()
    foreach ($i in (Get-ChildItem -LiteralPath "$($Letter):\" -Force -ErrorAction SilentlyContinue)) {
        $n = $i.Name
        if ($n -like 'limine*') { continue }
        if ($OurTopLevel -contains $n) {
            if ($n -eq 'EFI') {
                foreach ($j in (Get-ChildItem -LiteralPath $i.FullName -Force)) {
                    if ($j.Name -ne 'BOOT') { $bad += "EFI\$($j.Name)" }
                }
            }
            continue
        }
        $bad += $n
    }
    return ($bad -join ', ')
}

# Verdict on an existing volume for -Partition: '' = usable, else why not.
function Get-VolumeVerdict([string]$Letter) {
    $why = @()
    $vol = Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue
    if (-not $vol) { return "no volume $($Letter):" }
    if ($vol.FileSystem -ne 'FAT32') { $why += "file system $($vol.FileSystem), not FAT32" }
    if ([uint64]$vol.Size -lt $MinBytes) { $why += ('too small ({0:N1} GB; needs 1.5 GB)' -f ($vol.Size / 1GB)) }
    if ($Letter -eq (Get-SystemLetter)) { $why += 'the Windows volume' }
    $p = $null
    try { $p = Get-Partition -DriveLetter $Letter } catch { }
    if ($p) {
        $flags = @(Get-PartitionFlags $p)
        if ($flags.Count -gt 0) { $why += $flags }
    }
    if ($why.Count -eq 0) {
        $foreign = Get-ForeignFiles $Letter
        if ($foreign) { $why += "holds other files ($foreign)" }
    }
    return ($why -join '; ')
}

# ---- sidecar ----------------------------------------------------------------

function Find-Sidecar {
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        $f = Join-Path $d.RootDirectory.FullName $Sidecar
        try { if (Test-Path -LiteralPath $f) { return $f } } catch { }
    }
    return $null
}

function Read-Sidecar([string]$File) {
    try { return (Get-Content -LiteralPath $File -Raw | ConvertFrom-Json) } catch { return $null }
}

function Write-Utf8NoBom([string]$File, [string]$Text) {
    [System.IO.File]::WriteAllText($File, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

# ---- -Check -----------------------------------------------------------------

function Invoke-Check {
    $admin = Test-Admin
    Note 'atari-os-x86 install-windows.ps1 -Check (nothing is changed)'
    Note ''
    if ($admin) { Note 'Administrator: yes' } else { Note 'Administrator: no (installing needs an administrator PowerShell; some details below need it too)' }
    Note "Firmware: $(Get-FirmwareType)"
    switch (Get-SecureBootState) {
        'on' { Note 'Secure Boot: ON (turn it off in the firmware setup before installing)' }
        'off' { Note 'Secure Boot: off' }
        default { Note 'Secure Boot: unknown (needs administrator rights, or no UEFI)' }
    }
    $sys = Get-SystemLetter
    Note "BitLocker on $($sys): $(Get-BitLockerState "$($sys):") (shrinking a BitLocker volume is supported)"
    if (Test-FastStartup) { Note 'Fast Startup: on (do not change files on the ATARI drive from Windows and from the Atari between two starts; see README)' }
    else { Note 'Fast Startup: off' }
    try {
        $c = Get-Partition -DriveLetter $sys
        $s = Get-PartitionSupportedSize -DriveLetter $sys
        $maxShrink = [math]::Floor(([uint64]$c.Size - [uint64]$s.SizeMin) / 1MB)
        Note ('{0}: {1:N1} GB; it can shrink by up to {2} MB (-ShrinkMB 1536..32768)' -f $sys, ($c.Size / 1GB), $maxShrink)
    } catch {
        Note "$($sys): shrink limit unknown (needs administrator rights)"
    }
    Note ''
    Note 'Volumes (-Partition):'
    try {
        foreach ($v in (Get-Volume | Where-Object { $_.DriveLetter } | Sort-Object DriveLetter)) {
            $l = [string]$v.DriveLetter
            $verdict = Get-VolumeVerdict $l
            if (-not $verdict) { $verdict = 'ok' } else { $verdict = "no: $verdict" }
            Note ('  {0}:  {1,-8} {2,8:N1} GB  {3,-12} {4}' -f $l, $v.FileSystem, ($v.Size / 1GB), $v.FileSystemLabel, $verdict)
        }
    } catch {
        Note "  cannot list the volumes: $($_.Exception.Message)"
    }
    Note ''
    $sc = $null
    try { $sc = Find-Sidecar } catch { }
    if ($sc) {
        $d = Read-Sidecar $sc
        Note "Installed: $sc"
        if ($d) { Note "  firmware entry $($d.firmware), menu entry $($d.menu)" }
    } else {
        Note 'Installed: no Atari partition with atari-os\windows-install.json found'
    }
    if ($admin) {
        $fw = @(Find-OurEntries 'firmware')
        Note "Firmware entries named '$Name' with our loader: $(if ($fw.Count) { $fw -join ' ' } else { 'none' })"
        $bm = Get-BootMgrMenu
        if ($bm) { Note "Windows Boot Manager: timeout $($bm.Timeout), $($bm.Entries) entries in its list" }
    } else {
        Note 'Firmware entries: not readable without administrator rights (bcdedit)'
    }
    exit 0
}

# ---- -Remove ----------------------------------------------------------------

function Invoke-Remove {
    if (-not (Test-Admin) -and -not $WhatIfPreference) { Fail 'run this from an administrator PowerShell' }
    $sc = Find-Sidecar
    $d = $null
    if ($sc) { $d = Read-Sidecar $sc }
    $fw = @(); $mn = @(); $oldTimeout = $null
    if ($d) {
        if ($d.firmware) { $fw += [string]$d.firmware }
        if ($d.menu) { $mn += [string]$d.menu }
        if ($null -ne $d.oldTimeout) { $oldTimeout = [int]$d.oldTimeout }
    }
    # Without the sidecar (no drive letter, or deleted): entries with our
    # name and loader path.
    if (-not $d -and (Test-Admin)) {
        Note "No $Sidecar found; looking for boot entries named '$Name' that start $LoaderPath"
        $fw += @(Find-OurEntries 'firmware')
        $mn += @(Find-OurEntries 'bootapp')
    }
    if ($fw.Count -eq 0 -and $mn.Count -eq 0) {
        Note "No '$Name' entries of ours found; nothing to remove."
        exit 0
    }
    foreach ($g in $fw) {
        if (-not (Test-BcdEntry $g) -and -not $WhatIfPreference) { continue }
        if ($PSCmdlet.ShouldProcess("firmware boot entry $g", 'bcdedit /delete')) {
            Invoke-Bcd @('/delete', $g) | Out-Null
            Say "Deleted the firmware boot entry $g"
        }
    }
    foreach ($g in $mn) {
        if (-not (Test-BcdEntry $g) -and -not $WhatIfPreference) { continue }
        if ($PSCmdlet.ShouldProcess("Windows Boot Manager entry $g", 'bcdedit /delete')) {
            Invoke-Bcd @('/delete', $g) | Out-Null
            Say "Deleted the Windows Boot Manager entry $g"
        }
    }
    if ($null -ne $oldTimeout -and $mn.Count -gt 0) {
        if ($PSCmdlet.ShouldProcess('Windows Boot Manager', "bcdedit /timeout $oldTimeout (as before -Menu)")) {
            Invoke-Bcd @('/timeout', [string]$oldTimeout) | Out-Null
            Say "Windows Boot Manager timeout back to $oldTimeout s"
        }
    }
    if ($sc -and $PSCmdlet.ShouldProcess($sc, 'delete')) { Remove-Item -LiteralPath $sc -Force }
    if ($sc) { Note "The files stay on $($sc.Substring(0, 2)) (delete the partition in Disk Management if you want the space back)." }
    exit 0
}

# ---- install ----------------------------------------------------------------

function Read-Yes([string]$Prompt) {
    $a = Read-Host $Prompt
    if ($a -ne 'yes') { Note 'Cancelled, nothing changed.'; exit 1 }
}

function Invoke-Install {
    $admin = Test-Admin
    if (-not $admin -and -not $WhatIfPreference) { Fail 'run this from an administrator PowerShell (right-click > Run as administrator); -WhatIf and -Check work without' }
    if (-not $admin) { Warn 'not an administrator: this plan is incomplete (no partition sizes or boot entries)' }

    $fwType = Get-FirmwareType
    if ($fwType -eq 'BIOS') { Fail 'this Windows starts in BIOS (legacy) mode; the dual-boot installer is for UEFI machines. Use the USB stick (README, Install).' }
    if ($fwType -ne 'UEFI') { Warn "cannot tell whether this machine boots in UEFI mode ($fwType); continuing" }
    switch (Get-SecureBootState) {
        'on' { Fail 'Secure Boot is on; turn it off in the firmware setup (the Atari boot loader and kernel are unsigned), then run this again.' }
        'unknown' { Warn 'cannot read the Secure Boot state; if it is on, the Atari will not start (turn it off in the firmware setup)' }
    }
    $need = Test-Zip $Zip
    $zipFull = (Resolve-Path -LiteralPath $Zip).ProviderPath
    $sys = Get-SystemLetter

    # The target volume
    $letter = $null
    if ($PSCmdlet.ParameterSetName -eq 'Partition') {
        $letter = $Partition.Trim().TrimEnd('\').TrimEnd(':').ToUpper()
        if ($letter -notmatch '^[A-Z]$') { Fail "-Partition takes a drive letter like E:, not '$Partition'" }
        $verdict = Get-VolumeVerdict $letter
        if ($verdict) { Fail "$($letter): cannot be used: $verdict. Format it as FAT32 in Disk Management first (1.5 GB or more), or use -ShrinkMB." }
        $vol = Get-Volume -DriveLetter $letter
        if ([uint64]$vol.SizeRemaining -lt $need + 16MB) { Fail ('{0}: has {1:N0} MB free; the files need {2:N0} MB' -f $letter, ($vol.SizeRemaining / 1MB), ($need / 1MB)) }
        try {
            $disk = Get-Partition -DriveLetter $letter | Get-Disk
            if ([string]$disk.BusType -eq 'USB') { Warn "$($letter): is on a USB drive ($($disk.FriendlyName)); the Atari entry only works while it is plugged in" }
        } catch { }
        Note 'Plan:'
        Note ("  - copy the files ({0:N0} MB) from {1} to {2}: (FAT32 '{3}', {4:N1} GB)" -f ($need / 1MB), (Split-Path -Leaf $zipFull), $letter, $vol.FileSystemLabel, ($vol.Size / 1GB))
    } else {
        $c = $null; $sup = $null
        try {
            $c = Get-Partition -DriveLetter $sys
            $sup = Get-PartitionSupportedSize -DriveLetter $sys
        } catch {
            if (-not $WhatIfPreference) { Fail "cannot read the size limits of $($sys): $($_.Exception.Message)" }
        }
        $shrink = [uint64]$ShrinkMB * [uint64]1048576
        Note 'Plan:'
        if ($c -and $sup) {
            $newSize = [uint64]$c.Size - $shrink
            if ($newSize -lt [uint64]$sup.SizeMin) {
                Fail ('{0}: can shrink by at most {1:N0} MB right now (Windows says so; free space, page file and unmovable files decide). Try a smaller -ShrinkMB, or free space and defragment first.' -f $sys, (([uint64]$c.Size - [uint64]$sup.SizeMin) / 1MB))
            }
            $disk = Get-Disk -Number $c.DiskNumber
            Note ('  - shrink {0}: from {1:N1} GB to {2:N1} GB (disk {3}, {4}, {5})' -f $sys, ($c.Size / 1GB), ($newSize / 1GB), $c.DiskNumber, $disk.FriendlyName, $disk.PartitionStyle)
            Note "    BitLocker on $($sys): $(Get-BitLockerState "$($sys):")"
        } else {
            Note "  - shrink $($sys): by $ShrinkMB MB"
        }
        Note "  - create a $ShrinkMB MB partition in the freed space, format it FAT32, label ATARI, with a drive letter"
        Note ('  - copy the files ({0:N0} MB) from {1} onto it' -f ($need / 1MB), (Split-Path -Leaf $zipFull))
    }
    Note '  - add atari.media=<its serial> to its limine.conf'
    Note "  - firmware: add the boot entry '$Name' ($LoaderPath on that partition) at the END of the boot list; the default stays Windows"
    if ($Menu) { Note "  - Windows Boot Manager: add '$Name' to its menu (the blue chooser then shows at every start)" }
    if ($BootNow) { Note '  - start the Atari once at the next restart (BootNext), then restart now' }
    if (Test-FastStartup) { Warn 'Fast Startup is on: Windows keeps the ATARI drive open over a shutdown. Do not change files on it from Windows and from the Atari between two starts (or turn Fast Startup off: Control Panel > Power Options > Choose what the power buttons do).' }
    Note ''
    if ($WhatIfPreference) {
        Note 'What if: nothing is changed (-WhatIf).'
        exit 0
    }

    if ($PSCmdlet.ParameterSetName -eq 'Partition') {
        $typed = Read-Host "Type the drive letter ($($letter):) to confirm"
        if ($typed.Trim().TrimEnd('\').ToUpper() -ne "$($letter):") { Note "That is not $($letter):. Cancelled, nothing changed."; exit 1 }
        Read-Yes 'Copy the files and add the boot entry? Type yes'
    } else {
        Read-Yes "Shrink $($sys): by $ShrinkMB MB, make the ATARI partition and add the boot entry? Type yes"
        # Shrink, then create and format. After the shrink nothing is undone
        # automatically: C: can be extended back in Disk Management.
        if ($PSCmdlet.ShouldProcess("$($sys):", "Resize-Partition to $newSize bytes")) {
            Say "Shrinking $($sys): (this can take a few minutes)"
            Resize-Partition -DriveLetter $sys -Size $newSize
        }
        try {
            $np = $null
            if ($PSCmdlet.ShouldProcess("disk $($c.DiskNumber)", 'New-Partition in the freed space')) {
                Say 'Creating the ATARI partition'
                if ([string]$disk.PartitionStyle -eq 'GPT') {
                    $np = New-Partition -DiskNumber $c.DiskNumber -UseMaximumSize -AssignDriveLetter -GptType $BasicDataGpt
                } else {
                    $np = New-Partition -DiskNumber $c.DiskNumber -UseMaximumSize -AssignDriveLetter -MbrType FAT32
                }
            }
            if ([uint64]$np.Size -gt $shrink + 64MB) {
                throw ('the new partition is {0:N0} MB, more than the {1} MB freed: there was other free space on the disk. Delete it in Disk Management if that is not wanted.' -f ($np.Size / 1MB), $ShrinkMB)
            }
            if ($PSCmdlet.ShouldProcess("partition $($np.PartitionNumber) on disk $($c.DiskNumber)", 'Format-Volume FAT32 ATARI')) {
                Say 'Formatting it FAT32'
                $np | Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'ATARI' -AllocationUnitSize 4096 -Confirm:$false -Force | Out-Null
            }
            $np = Get-Partition -DiskNumber $c.DiskNumber -PartitionNumber $np.PartitionNumber
            $letter = [string]$np.DriveLetter
            if (-not $letter -or $letter -eq "`0") { throw 'the new partition got no drive letter (assign one in Disk Management, then run with -Partition X:)' }
        } catch {
            Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "$($sys): was shrunk by $ShrinkMB MB. Nothing else is undone automatically: in Disk Management, delete the new partition (if any) and use 'Extend Volume' on $($sys): to get the space back, or make the partition FAT32 yourself and run this script with -Partition X:." -ForegroundColor Red
            exit 1
        }
    }

    try {
        # Copy (Expand-Archive overwrites an earlier install's files; your
        # atari\ files that are not in the zip stay).
        if ($PSCmdlet.ShouldProcess("$($letter):\", "Expand-Archive $zipFull")) {
            Say "Copying the files to $($letter): (a minute or two)"
            $oldProgress = $ProgressPreference
            $ProgressPreference = 'SilentlyContinue'   # 5.1's progress bar makes it 10x slower
            try { Expand-Archive -LiteralPath $zipFull -DestinationPath "$($letter):\" -Force }
            finally { $ProgressPreference = $oldProgress }
        }
        # atari.media=<serial> on every cmdline: (what the F8 disk installer does)
        $ld = Get-CimInstance -ClassName Win32_LogicalDisk -Filter "DeviceID='$($letter):'"
        $hex = ([string]$ld.VolumeSerialNumber).ToUpper()
        if ($hex -notmatch '^[0-9A-F]{8}$') { throw "unexpected volume serial '$hex' of $($letter):" }
        $serial = $hex.Substring(0, 4) + '-' + $hex.Substring(4, 4)
        $conf = "$($letter):\limine.conf"
        $text = [System.IO.File]::ReadAllText($conf)
        $lines = $text -split "`n"
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $l = $lines[$i].TrimEnd("`r")
            if ($l -match '^\s*cmdline:' -and $l -notmatch 'atari\.media=') { $lines[$i] = "$l atari.media=$serial" }
            else { $lines[$i] = $l }
        }
        Write-Utf8NoBom $conf ($lines -join "`n")
        Say "limine.conf: atari.media=$serial"

        # The firmware entry: reuse ours (sidecar, or same name and loader)
        $scFile = "$($letter):\$Sidecar"
        $old = $null
        if (Test-Path -LiteralPath $scFile) { $old = Read-Sidecar $scFile }
        $guid = $null
        if ($old -and $old.firmware -and (Test-BcdEntry ([string]$old.firmware))) { $guid = [string]$old.firmware }
        if (-not $guid) {
            $existing = @(Find-OurEntries 'firmware')
            if ($existing.Count -gt 0) { $guid = $existing[0] }
        }
        if ($guid) {
            Say "Reusing the firmware boot entry $guid"
        } else {
            $guid = Get-GuidFrom (Invoke-Bcd @('/copy', '{bootmgr}', '/d', $Name))
            Say "Created the firmware boot entry $guid"
        }
        Invoke-Bcd @('/set', $guid, 'description', $Name) | Out-Null
        Invoke-Bcd @('/set', $guid, 'device', "partition=$($letter):") | Out-Null
        Invoke-Bcd @('/set', $guid, 'path', $LoaderPath) | Out-Null
        # The one firmware change: append to the boot list, never first.
        Invoke-Bcd @('/set', '{fwbootmgr}', 'displayorder', $guid, '/addlast') | Out-Null
        Say "'$Name' is the last entry of the firmware boot list"

        $menuGuid = $null
        $oldTimeout = $null
        if ($old -and $null -ne $old.oldTimeout) { $oldTimeout = [int]$old.oldTimeout }
        if ($Menu) {
            if ($old -and $old.menu -and (Test-BcdEntry ([string]$old.menu))) { $menuGuid = [string]$old.menu }
            $bm = Get-BootMgrMenu
            if (-not $menuGuid) {
                $menuGuid = Get-GuidFrom (Invoke-Bcd @('/create', '/d', $Name, '/application', 'bootapp'))
            }
            Invoke-Bcd @('/set', $menuGuid, 'device', "partition=$($letter):") | Out-Null
            Invoke-Bcd @('/set', $menuGuid, 'path', $LoaderPath) | Out-Null
            Invoke-Bcd @('/displayorder', $menuGuid, '/addlast') | Out-Null
            Say "'$Name' is in the Windows Boot Manager menu ($menuGuid)"
            if ($bm -and $null -ne $bm.Timeout -and ($bm.Timeout -eq 0 -or $bm.Entries -eq 1)) {
                if ($null -eq $oldTimeout) { $oldTimeout = $bm.Timeout }
                if ($bm.Timeout -lt 10) {
                    Invoke-Bcd @('/timeout', '10') | Out-Null
                    Say "Windows Boot Manager timeout set to 10 s (was $($bm.Timeout); -Remove puts it back)"
                }
            } elseif (-not $bm -or $null -eq $bm.Timeout) {
                Warn 'could not read the Boot Manager timeout; it is left as it is'
            }
            Note 'The blue Windows chooser now shows at every start; -Remove takes it away again.'
        } elseif ($old -and $old.menu) {
            $menuGuid = [string]$old.menu   # kept from an earlier -Menu install
        }

        $data = New-Object PSObject -Property ([ordered]@{
            name = $Name; firmware = $guid; menu = $menuGuid; oldTimeout = $oldTimeout
            serial = $serial; installed = (Get-Date).ToString('s')
        })
        New-Item -ItemType Directory -Force -Path "$($letter):\atari-os" | Out-Null
        Write-Utf8NoBom $scFile ($data | ConvertTo-Json)

        if ($BootNow) {
            Invoke-Bcd @('/set', '{fwbootmgr}', 'bootsequence', $guid) | Out-Null
            Say "The next restart starts '$Name' once; after that Windows is the default again."
        }
    } catch {
        Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "The ATARI partition $($letter): stays as it is; run the script again, or -Remove to drop any entry it made." -ForegroundColor Red
        exit 1
    }

    Note ''
    Say "Done. The files are on $($letter): (an ordinary FAT32 drive: your Atari library goes into $($letter):\atari\library)."
    Note "To start the Atari: open the firmware's boot menu at power-on (F12, F9, F8 or Esc, depending on the maker) and pick '$Name'."
    Note 'Secure Boot must stay off. To remove the entry: .\install-windows.ps1 -Remove (the files stay).'
    if ($BootNow) { Restart-Computer -Confirm }
    exit 0
}

# ---- main -------------------------------------------------------------------

switch ($PSCmdlet.ParameterSetName) {
    'Remove' { Invoke-Remove }
    'Partition' { Invoke-Install }
    'Shrink' { Invoke-Install }
    default { Invoke-Check }
}
