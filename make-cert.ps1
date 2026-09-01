# ============================================================================
# make-cert.ps1 - Self-signed local CA + gateway HTTPS certificate.
#
# Why: browsers expose getUserMedia (microphone) ONLY on secure contexts.
# Serving the gateway over HTTPS (with a root CA trusted on the tablet) turns
# https://<gateway-ip>:15433/ into a secure context - no Cordova/APK needed.
#
# What it does:
#   1. Creates OR reuses the local root CA ("VTA Local Root CA", 15 years).
#      Reuse matters: tablets install the CA ONCE; re-issuing the server
#      certificate later (e.g. gateway IP changed) needs no tablet action.
#   2. Issues a server cert whose SAN covers ALL current local IPv4
#      addresses (+ any extra IPs passed as arguments) and "localhost".
#      Browsers require the IP inside SAN; CommonName alone is ignored.
#   3. Exports:
#        certs\ca.crt      -> install on tablets (Android: Settings >
#                             Security > More security settings > Encryption
#                             & credentials > Install a certificate > CA cert)
#        certs\gateway.pfx -> loaded automatically by Program.cs (port 15433)
#
# Usage (double-click make-cert.bat, or):
#   powershell -NoProfile -ExecutionPolicy Bypass -File make-cert.ps1 [IPs...]
#   e.g.  powershell -NoProfile -ExecutionPolicy Bypass -File make-cert.ps1 192.168.1.10
#
# NOTE: the certificate binds to IPs. If the gateway IP changes, re-run this
# script and restart the service. Tablets need NO reinstall (same root CA).
# ============================================================================
param(
    [string[]]$ExtraIp = @(),
    [string]$OutDir = (Join-Path $PSScriptRoot 'certs'),
    [string]$PfxPassword = 'vta-local-2026'
)
$ErrorActionPreference = 'Stop'
$caCn   = 'CN=VTA Local Root CA'
$leafCn = 'CN=VoiceTableAssist Gateway'
$store  = 'Cert:\CurrentUser\My'

# ---- 1) Collect IPv4 addresses (skip loopback / APIPA / unspecified) ----
$ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '0.0.0.0' } |
    Select-Object -ExpandProperty IPAddress -Unique)
foreach ($e in $ExtraIp) {
    if ($e -and $ips -notcontains $e) { $ips += $e }
}
if ($ips.Count -eq 0) {
    throw 'No usable IPv4 address found. Pass one explicitly: make-cert.bat 192.168.1.10'
}
Write-Host ("SAN IPs  : " + ($ips -join ', '))

# ---- 2) Create or reuse the root CA ----
$ca = Get-ChildItem $store -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq $caCn -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if ($ca) {
    Write-Host ("Root CA  : reused (thumbprint " + $ca.Thumbprint + ")")
} else {
    $ca = New-SelfSignedCertificate -Subject $caCn `
        -KeyExportPolicy Exportable -KeySpec Signature -KeyLength 4096 `
        -HashAlgorithm SHA256 -KeyUsage CertSign `
        -TextExtension @('2.5.29.19={critical}{text}ca=true') `
        -CertStoreLocation $store -NotAfter (Get-Date).AddYears(15)
    Write-Host ("Root CA  : created (thumbprint " + $ca.Thumbprint + ", 15 years)")
}

# ---- 3) Issue the server certificate (SAN = all IPs + localhost) ----
# Drop previous leaves with the same subject to keep the store clean.
Get-ChildItem $store -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq $leafCn -and $_.Thumbprint -ne $ca.Thumbprint } |
    Remove-Item -Force
# SAN text-extension syntax: 2.5.29.17={text}IPAddress=a&IPAddress=b&DNS=localhost
$sanText = ((($ips | ForEach-Object { "IPAddress=$_" }) + 'DNS=localhost') -join '&')
$leaf = New-SelfSignedCertificate -Subject $leafCn `
    -Signer $ca -KeyExportPolicy Exportable -KeySpec KeyExchange -KeyLength 2048 `
    -HashAlgorithm SHA256 -KeyUsage DigitalSignature,KeyEncipherment `
    -TextExtension @("2.5.29.17={text}$sanText") `
    -CertStoreLocation $store -NotAfter (Get-Date).AddYears(10)
Write-Host ("Leaf cert: created (thumbprint " + $leaf.Thumbprint + ", 10 years)")

# ---- 4) Export ca.crt (PEM, for tablets) + gateway.pfx (for Kestrel) ----
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$caDer = Join-Path $OutDir 'ca.der'
$caPem = Join-Path $OutDir 'ca.crt'
Export-Certificate -Cert $ca -FilePath $caDer -Type CERT | Out-Null
certutil -encode $caDer $caPem | Out-Null    # DER -> Base64 PEM, Android-friendly
Remove-Item $caDer -Force
$pfxPath = Join-Path $OutDir 'gateway.pfx'
$secPwd  = ConvertTo-SecureString -String $PfxPassword -Force -AsPlainText
Export-PfxCertificate -Cert $leaf -FilePath $pfxPath -Password $secPwd | Out-Null

# 同步一份 ca.crt 到 wwwroot：手机/平板连上 http://<网关IP>:15232/ca.crt 即可直接下载安装。
$www = Join-Path $PSScriptRoot 'wwwroot'
if (Test-Path $www) {
    Copy-Item -Force $caPem (Join-Path $www 'ca.crt')
    Write-Host '   ca.crt 已同步到 wwwroot/（手机可 http://<网关IP>:15232/ca.crt 直接下载）'
}

Write-Host ''
Write-Host '============================================================'
Write-Host ' DONE. Files written:'
Write-Host ("   " + $caPem)
Write-Host ("   " + $pfxPath)
Write-Host ' Next steps:'
Write-Host '   1. Restart VoiceTableAssist -> HTTPS on port 15433.'
Write-Host '   2. Tablet (once): copy ca.crt over, then Settings > Security'
Write-Host '      > Encryption & credentials > Install a certificate >'
Write-Host '      CA certificate. Open https://<gateway-ip>:15433/'
Write-Host '   3. Gateway IP changed? Re-run this script + restart service.'
Write-Host '      Tablets need NO reinstall (same root CA).'
Write-Host '============================================================'
