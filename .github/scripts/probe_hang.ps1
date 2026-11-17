# Run an executable that hangs on the Windows runner; if it outlives the wait, dump every thread's
# stack with cdb (Windows SDK debugger) and publish it as an annotation, then kill the process.
# Usage: probe_hang.ps1 -Exe <path> [-Arguments <string>] [-Seconds 30]
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [string]$Arguments = "",
  [int]$Seconds = 30
)

function Publish([string]$level, [string]$title, [string]$text) {
  if ($text.Length -gt 3500) { $text = $text.Substring(0, 3500) }
  $text = $text -replace '%', '%25' -replace "`r", '' -replace "`n", '%0A'
  Write-Output "::${level} title=${title}::${text}"
}

$name = Split-Path $Exe -Leaf
if (-not (Test-Path $Exe)) { Publish "warning" "hang probe $name" "missing: $Exe"; exit 0 }

$out = New-TemporaryFile
$err = New-TemporaryFile
$start = @{ FilePath = $Exe; PassThru = $true; NoNewWindow = $true
  RedirectStandardOutput = $out; RedirectStandardError = $err }
if ($Arguments) { $start.ArgumentList = $Arguments }  # an empty ArgumentList is rejected
$process = Start-Process @start
$null = $process.Handle  # without a cached handle ExitCode reads empty after the exit
if ($process.WaitForExit($Seconds * 1000)) {
  $tail = (Get-Content $out -Tail 5) -join "`n"
  Publish "notice" "hang probe $name" ("exited with 0x{0:X8} in under ${Seconds} s`n{1}" -f $process.ExitCode, $tail)
  exit 0
}

$cdb = "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe"
if (Test-Path $cdb) {
  # .symfix: system DLL symbols from the Microsoft server; our Release binaries carry no PDB.
  # qd detaches without killing, so the stacks are of the process as it hung.
  $stacks = & $cdb -p $process.Id -c ".symfix;.reload;~*kc 25;qd" 2>&1 | Out-String
  $stacks = ($stacks -split "`n" | Where-Object { $_ -match '^\s*(#|\.|\s*\d+\s+Id:|[0-9a-f]{2} )|!|\+0x' }) -join "`n"
  Publish "error" "hang probe $name (alive after ${Seconds} s)" $stacks
} else {
  Publish "error" "hang probe $name (alive after ${Seconds} s)" "cdb not found at $cdb"
}
Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
