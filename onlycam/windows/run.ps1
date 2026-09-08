param(
    [string]$Url = "http://192.168.0.10:8080/stream",
    [switch]$VirtualCam
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Test-Path ".venv")) {
    python -m venv .venv
}

& .\.venv\Scripts\python.exe -m pip install -r requirements.txt
$argsList = @("--url", $Url)
if ($VirtualCam) { $argsList += "--virtual-cam" }
& .\.venv\Scripts\python.exe .\receiver.py @argsList
