# syscheck.ps1 — état système Windows pertinent pour l'audio temps réel
# Usage : powershell -ExecutionPolicy Bypass -File tools\perf\scripts\syscheck.ps1 [-Dll chemin\at_wavespace_engine.dll]
param([string]$Dll = "")

Write-Host "=== CPU ===" -ForegroundColor Cyan
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed, CurrentClockSpeed | Format-List

Write-Host "=== Plan d'alimentation ===" -ForegroundColor Cyan
powercfg /getactivescheme
Write-Host "(Recommandé pour la mesure : 'Performances élevées' / 'Performances optimales', secteur branché)"
$ac = (Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue)
if ($ac) { $ac | Select-Object PowerOnline, Discharging | Format-List }

Write-Host "=== Core parking / état min processeur (secteur) ===" -ForegroundColor Cyan
powercfg /query SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN | Select-String "Current AC|Paramètre actuel"
powercfg /query SCHEME_CURRENT SUB_PROCESSOR CPMINCORES | Select-String "Current AC|Paramètre actuel"

Write-Host "=== Résolution du timer système ===" -ForegroundColor Cyan
Write-Host "Utiliser l'option --timer-res du bench pour comparer."

Write-Host "=== Pilotes audio ASIO installés ===" -ForegroundColor Cyan
Get-ChildItem "HKLM:\SOFTWARE\ASIO" -ErrorAction SilentlyContinue | ForEach-Object { $_.PSChildName }

Write-Host "=== Defender : exclusion du dossier de travail ? ===" -ForegroundColor Cyan
try { (Get-MpPreference).ExclusionPath } catch { Write-Host "non lisible (droits admin requis)" }

if ($Dll -ne "") {
    Write-Host "=== DLL moteur : runtime lié (Debug = ucrtbased / VCRUNTIME140D / MSVCP140D) ===" -ForegroundColor Cyan
    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($dumpbin) {
        $deps = & dumpbin /dependents $Dll | Select-String "\.dll"
        $deps
        if ($deps -match "ucrtbased|140D") { Write-Host "!!! DLL DEBUG : mesures non représentatives !!!" -ForegroundColor Red }
        else { Write-Host "Runtime Release : OK" -ForegroundColor Green }
    } else { Write-Host "dumpbin introuvable : lancer depuis Developer PowerShell for VS 2022" }
}

Write-Host "`n=== À faire à la main ===" -ForegroundColor Cyan
Write-Host "- LatencyMon (resplendence.com) pendant 5 min de lecture : DPC/ISR max, pilote incriminé."
Write-Host "- Panneau du pilote ASIO : noter la taille de buffer RÉELLEMENT réglée."
Write-Host "- Gestionnaire des tâches > Détails : le process audio est-il marqué 'Mode efficacité' ?"
