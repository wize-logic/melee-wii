# Melee for the Wii: copies your Slippi account (user.json, from Slippi
# Launcher on this PC) to the Wii's SD card or USB drive, for Slippi Online on
# the Wii to log in with. Started by "Copy my Slippi account (Melee).bat". Only
# that one file is copied, and only to that drive; nothing is sent anywhere.
param([string]$Here = '')
$ErrorActionPreference = 'Stop'

function Stop-Copy($Message) {
    Write-Host ''
    Write-Host $Message -ForegroundColor Yellow
    exit 1
}

function Read-Account($Path) {
    try {
        $a = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path | ConvertFrom-Json
    } catch { return $null }
    if (-not $a.uid -or -not $a.playKey -or -not $a.connectCode) { return $null }
    return $a
}

function Describe($a) {
    $name = if ($a.displayName) { $a.displayName } else { '(no name)' }
    return "$name ($($a.connectCode))"
}

Write-Host 'Melee for the Wii: copy your Slippi account to the Wii''s SD card or USB drive'
Write-Host '(Unofficial: not made by Project Slippi or Nintendo.)'
Write-Host ''

# 1. The account: the one Slippi Launcher on this PC is logged in to.
$candidates = @()
foreach ($sub in @('netplay', 'netplay-beta')) {
    $p = Join-Path $env:APPDATA "Slippi Launcher\$sub\User\Slippi\user.json"
    if (Test-Path -LiteralPath $p) { $candidates += $p }
}
$accounts = @()
foreach ($p in $candidates) {
    $a = Read-Account $p
    if ($a -and -not ($accounts | Where-Object { $_.Account.uid -eq $a.uid })) {
        $accounts += [pscustomobject]@{ Path = $p; Account = $a }
    }
}
if ($accounts.Count -eq 0) {
    Stop-Copy ("No Slippi account was found on this PC.`n" +
        "  1. Install Slippi Launcher (https://slippi.gg) and log in (or create an account).`n" +
        "  2. Click Play once, so it sets up Slippi Dolphin.`n" +
        "  3. Run this again.")
}
$pick = $accounts[0]
if ($accounts.Count -gt 1) {
    Write-Host 'Slippi accounts on this PC:'
    for ($i = 0; $i -lt $accounts.Count; $i++) { Write-Host "  $($i + 1). $(Describe $accounts[$i].Account)" }
    $n = Read-Host 'Type the number of the account for the Wii and press Enter'
    if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $accounts.Count) { $pick = $accounts[[int]$n - 1] }
}

# 2. The Wii's drive (an SD card or a USB drive): the Wii reads only
#    \apps\melee at the top of the drive it starts Melee from, so the account
#    goes there. Where this script is counts only when it is the top of a drive
#    (the zip extracted onto it); a folder on this PC or a folder inside the
#    drive is not where the Wii looks.
function Test-Card($Root) { Test-Path -LiteralPath (Join-Path $Root 'apps\melee\boot.dol') }
$cards = @()
$hereRoot = ''
if ($Here -and (Test-Path -LiteralPath $Here)) { $hereRoot = (Resolve-Path -LiteralPath $Here).Path }
if ($hereRoot -and [IO.Path]::GetPathRoot($hereRoot) -eq $hereRoot -and (Test-Card $hereRoot)) {
    $cards += $hereRoot
} else {
    if ($hereRoot) {
        Write-Host "This copy of Melee for the Wii is in $hereRoot," -ForegroundColor Yellow
        Write-Host 'which is not the top of an SD card or USB drive: the Wii would not see an account put here.' -ForegroundColor Yellow
        Write-Host 'Looking for the Wii''s SD card or USB drive instead...'
        Write-Host ''
    }
    $system = [IO.Path]::GetPathRoot($env:SystemRoot)
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        $root = $d.RootDirectory.FullName
        if ($d.IsReady -and $root -ne $system -and (Test-Card $root)) { $cards += $root }
    }
}
if ($cards.Count -eq 0) {
    Stop-Copy ("No SD card or USB drive with Melee for the Wii was found.`n" +
        "  1. Put the Wii's SD card (or USB drive) in this PC.`n" +
        "  2. Extract the release zip onto the top of it, so it has`n" +
        "     apps\melee\boot.dol (E:\apps\melee\boot.dol, not E:\melee-wii\apps\...).`n" +
        "  3. Run this again from that drive.")
}
$card = $cards[0]
if ($cards.Count -gt 1) {
    Write-Host 'Drives with Melee for the Wii:'
    for ($i = 0; $i -lt $cards.Count; $i++) { Write-Host "  $($i + 1). $($cards[$i])" }
    $n = Read-Host 'Type the number of the drive you start Melee from on the Wii and press Enter'
    if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $cards.Count) { $card = $cards[[int]$n - 1] }
}
try {
    $format = ([IO.DriveInfo]::new($card)).DriveFormat
    if ($format -and $format -notmatch '^FAT') {
        Write-Host "Note: $card is formatted $format. The Wii reads only FAT32 SD cards and USB drives." -ForegroundColor Yellow
    }
} catch {}

# 3. Copy (the previous account, if a different one, is kept beside it). The
#    bytes are Slippi Launcher's, less a byte order mark if an editor added one.
$bytes = [IO.File]::ReadAllBytes($pick.Path)
if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    $bytes = $bytes[3..($bytes.Length - 1)]
}
$folder = Join-Path $card 'apps\melee\slippi'
$dest = Join-Path $folder 'user.json'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
if (Test-Path -LiteralPath $dest) {
    $old = Read-Account $dest
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($dest)) -eq [Convert]::ToBase64String($bytes)) {
        Write-Host "Your account $(Describe $pick.Account) is already on $card."
        Write-Host 'Nothing to do: put it in your Wii and start Melee.'
        exit 0
    }
    $backup = "$dest.old-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Move-Item -LiteralPath $dest $backup
    if ($old) { Write-Host "$card had the account $(Describe $old): kept as $(Split-Path -Leaf $backup)." }
}
[IO.File]::WriteAllBytes($dest, [byte[]]$bytes)
try { Write-VolumeCache -DriveLetter $card[0] -ErrorAction SilentlyContinue } catch {}
$check = Read-Account $dest
if (-not $check -or $check.uid -ne $pick.Account.uid) {
    Stop-Copy "The account could not be written to $dest. Is it locked (an SD card's write-protect switch)?"
}
Write-Host ''
Write-Host "Done: your Slippi account $(Describe $pick.Account) is on $card." -ForegroundColor Green
Write-Host "  ($dest)"
Write-Host ''
Write-Host 'Next: safely eject it, put it in your Wii, and start Melee'
Write-Host 'from the Homebrew Channel. (Keep user.json private: it is your Slippi login.)'
exit 0
