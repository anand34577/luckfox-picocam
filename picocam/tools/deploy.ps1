<#
Install PicoCam on a Luckfox Pico board (Windows).

  .\tools\deploy.ps1                          # board at 172.32.0.93 (USB cable), newest release from GitHub
  .\tools\deploy.ps1 -Board 192.168.1.64      # board on your network
  .\tools\deploy.ps1 -Package .\picocam.tar.gz
  .\tools\deploy.ps1 -Lite                    # no demo models (boards with little flash)
  .\tools\deploy.ps1 -Build                   # compile from source first (WSL + Docker, see docs/BUILDING.md)

Needs the OpenSSH client (built into Windows 10/11). The board's default login is root / luckfox; ssh asks for the password once.
#>
param(
    [string]$Board = '172.32.0.93',
    [string]$Package = '',
    [switch]$Lite,
    [switch]$Models,
    [switch]$Build,
    [string]$Distro = 'Debian',
    [string]$Repo = 'anand34577/luckfox-picocam'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)   # ...\picocam
$name = if ($Models) { 'picocam-models' } elseif ($Lite) { 'picocam-lite' } else { 'picocam' }

if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
    throw 'The OpenSSH client is missing. Settings > Apps > Optional features > Add a feature > "OpenSSH Client", then run this again.'
}

if ($Build) {
    $wslRoot = (wsl -d $Distro wslpath -a ($root -replace '\\', '/')).Trim()
    Write-Host '== firmware + web UI + package (Docker in WSL)'
    wsl -d $Distro -- bash -lc "mkdir -p ~/picocam-build && docker run --rm -v '$wslRoot/..':/work -v ~/picocam-build:/tmp/picocam-build -w /work/picocam luckfox-ai:1.1 sh build.sh firmware"
    if ($LASTEXITCODE -ne 0) { throw 'firmware build failed' }
    Push-Location "$root\web"
    if (-not (Test-Path node_modules)) { npm ci --no-audit --no-fund }
    npm run build
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'web build failed' }
    Pop-Location
    wsl -d $Distro -- bash -lc "cd '$wslRoot' && bash tools/package.sh"
    if ($LASTEXITCODE -ne 0) { throw 'packaging failed' }
    $v = (Get-Content "$root\VERSION" -Raw).Trim()
    $suffix = if ($Lite) { '-lite' } else { '' }
    $Package = "$root\dist\picocam-$v$suffix.tar.gz"
}

if (-not $Package) {
    $Package = "$root\dist\$name.tar.gz"
    $url = "https://github.com/$Repo/releases/latest/download/$name.tar.gz"
    Write-Host "== downloading $name.tar.gz (latest release of github.com/$Repo)"
    New-Item -ItemType Directory -Force "$root\dist" | Out-Null
    try { Invoke-WebRequest -Uri $url -OutFile $Package -UseBasicParsing }
    catch { Remove-Item $Package -ErrorAction SilentlyContinue; throw 'Download failed. Check your internet connection, or build it yourself (-Build), or pass a file with -Package.' }
}
if (-not (Test-Path $Package)) { throw "package not found: $Package" }
$Package = (Resolve-Path $Package).Path
if ((Get-Item $Package).Length -lt 100000) { throw "$Package is too small to be a PicoCam package (interrupted download?). Delete it and run again." }

Write-Host "== installing $Package on $Board (password: luckfox, unless you changed it)"
# One ssh connection: the package is streamed through it (no scp/sftp needed on the board), unpacked and installed.
# Boards share the address 172.32.0.93 and get re-flashed, so a stale known_hosts entry must not block the installer.
$remote = 'rm -rf /userdata/pcinst && mkdir -p /userdata/pcinst && gunzip -c | tar -x -C /userdata/pcinst && sh /userdata/pcinst/install.sh; rc=$?; rm -rf /userdata/pcinst; exit $rc'
if ($Models) {
    $remote = 'mkdir -p /userdata/picocam/models && gunzip -c | tar -x -C /userdata/picocam/models && echo models installed'
}
cmd /c "ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR root@$Board `"$remote`" < `"$Package`""
$rc = $LASTEXITCODE
if ($rc -eq 255) {
    Write-Host ''
    Write-Host "!! Could not reach or log in to root@$Board." -ForegroundColor Red
    Write-Host '   - Over USB the board is 172.32.0.93. Your PC needs an address in that network (172.32.0.100, mask 255.255.255.0).'
    Write-Host '   - On a network, use the board IP: .\tools\deploy.ps1 -Board <ip>'
    Write-Host '   - Default password is luckfox. After a PicoCam first start it is the admin password you chose.'
    Write-Host '   See docs\TROUBLESHOOTING.md'
    exit 1
}
if ($rc -ne 0) { Write-Host "!! The installer reported an error (exit $rc), see above. Nothing else was changed." -ForegroundColor Red; exit $rc }
Write-Host ''
if ($Models) { Write-Host 'Done. Open AI > Models in the web UI: the demo models are listed there.' } else { Write-Host "Done. Open http://$Board/ and set the admin password." }
