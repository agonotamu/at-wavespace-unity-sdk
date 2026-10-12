# run_matrix.ps1 — balaye les configurations du banc et écrit un CSV par série.
# Usage :
#   powershell -ExecutionPolicy Bypass -File tools\perf\scripts\run_matrix.ps1 `
#       -Dll _at_wavespace_engine\Builds\VisualStudio2022_Probe\Release\at_wavespace_engine.dll -Device "Nom du périphérique" -Quick
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [string]$Device = "",
    [int]$Out = 32,
    [string]$Bench = "tools\perf\bench\build\Release\at_spat_bench.exe",
    [string]$Signals = "signals",
    [string]$Sources = "1,2,4,6,8,10,12,16,24",
    [int]$Duration = 15,
    [switch]$Quick,  # ne joue que les séries essentielles
    [switch]$Downmix,            # carte son stéréo : $Out HP virtuels rendus sur 2 sorties
    [string]$Render = "amp",     # amp | hrtf (avec -Downmix)
    [double]$MasterDb = -12
)

$stamp = Get-Date -Format "yyyyMMdd_HHmm"
$outDir = "results\$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$pink = "$Signals\pink_10s.wav"

function Run($label, $extra) {
    $csv = "$outDir\$label.csv"
    Write-Host "`n### $label ###" -ForegroundColor Cyan
    $benchArgs = @("--dll", $Dll, "--out", $Out, "--sources", $Sources, "--duration", $Duration,
              "--csv", $csv, "--label", $label) + $extra
    if ($Device -ne "") { $benchArgs += @("--device", $Device) }
    if ($Downmix) { $benchArgs += @("--downmix", "--render", $Render) }
    $benchArgs += @("--master-db", $MasterDb)
    & $Bench @benchArgs 2>&1 | Tee-Object -FilePath "$outDir\$label.log"
}

# H0 : le buffer change-t-il vraiment ?
foreach ($b in 256, 512, 1024) { Run "buf$b`_static" @("--buffer", $b, "--file", $pink, "--motion", "static") }
if ($Quick) {
    Run "buf512_nomt_blockwfs" @("--buffer", 512, "--file", $pink, "--perf", "nomt,blockwfs")
    Write-Host "`nRésultats : $outDir"; exit
}

# H4 : mouvement / fréquence d'envoi des transforms
Run "buf512_orbit60"  @("--buffer", 512, "--file", $pink, "--motion", "orbit", "--update-hz", 60)
Run "buf512_noupdate" @("--buffer", 512, "--file", $pink, "--motion", "static", "--update-hz", 0)

# H2 : dénormaux
Run "buf512_decay"     @("--buffer", 512, "--file", "$Signals\decay_to_silence.wav")
Run "buf512_decay_ftz" @("--buffer", 512, "--file", "$Signals\decay_to_silence.wav", "--perf", "ftz")

# H3 : lecture fichier
Run "buf512_longfile" @("--buffer", 512, "--file", "$Signals\pink_120s.wav")
Run "buf512_noread"   @("--buffer", 512, "--file", $pink, "--perf", "noread")

# H5 / H7 : kill switches
Run "buf512_mutelog" @("--buffer", 512, "--file", $pink, "--perf", "mutelog")
Run "buf512_nonfc"   @("--buffer", 512, "--file", $pink, "--perf", "nonfc")
Run "buf512_nomask"  @("--buffer", 512, "--file", $pink, "--perf", "nomask")
Run "buf512_freeze"  @("--buffer", 512, "--file", $pink, "--perf", "freeze", "--motion", "orbit")

# Audit statique : multithreading (dès 3 joueurs) et recalcul WFS par échantillon
Run "buf512_nomt"          @("--buffer", 512, "--file", $pink, "--perf", "nomt")
Run "buf512_blockwfs"      @("--buffer", 512, "--file", $pink, "--perf", "blockwfs")
Run "buf512_nomt_blockwfs" @("--buffer", 512, "--file", $pink, "--perf", "nomt,blockwfs")
Run "buf512_noprefilter"   @("--buffer", 512, "--file", $pink, "--prefilter", 0)

# H6 : ordonnancement
Run "buf512_pcore"     @("--buffer", 512, "--file", $pink, "--affinity", "pcore")
Run "buf512_ecore"     @("--buffer", 512, "--file", $pink, "--affinity", "ecore")
Run "buf512_nothrottle_high" @("--buffer", 512, "--file", $pink, "--no-throttle", "--priority", "high", "--timer-res")

Write-Host "`nRésultats : $outDir  →  python tools\perf\scripts\analyze.py $outDir\*.csv"
