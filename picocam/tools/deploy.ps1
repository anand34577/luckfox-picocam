<#
Install PicoCam on a Luckfox Pico board (Windows).

  .\tools\deploy.ps1                          # board at 172.32.0.93 (USB cable), newest release from GitHub
  .\tools\deploy.ps1 -Board 192.168.1.64      # board on your network
  .\tools\deploy.ps1 -Package .\picocam-1.2.0-lite.tar.gz
  .\tools\deploy.ps1 -Lite                    # no demo models (boards with little flash)
  .\tools\deploy.ps1 -Build                   # compile from source first (WSL + Docker, see docs/BUILDING.md)

Needs the OpenSSH client (built into Windows 10/11). The board's default login is root / luckfox; ssh asks for the password.
#>
param(
    [string]$Board = '172.32.0.93',
    [string]$Package = '',
    [switch]$Lite,
    [switch]$Build,
    [string]$Distro = 'Debian',
    [string]$Repo = 'anand34577/luckfox-picocam'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)   # ...\picocam
$version = (Get-Content "$root\VERSION" -Raw).Trim()
$suffix = if ($Lite) { '-lite' } else { '' }

if ($Build) {
    $wslRoot = (wsl -d $Distro wslpath -a ($root -replace '\', '/')).Trim()
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
}

if (-not $Package) {
    $Package = "$root\dist\picocam-$version$suffix.tar.gz"
    if (-not (Test-Path $Package)) {
        $url = "https://github.com/$Repo/releases/latest/download/picocam-$version$suffix.tar.gz"
        Write-Host "== downloading $url"
        New-Item -ItemType Directory -Force "$root\dist" | Out-Null
        try { Invoke-WebRequest -Uri $url -OutFile $Package -UseBasicParsing }
        catch { Remove-Item $Package -ErrorAction SilentlyContinue; throw "download failed; build it yourself (-Build) or pass -Package" }
    }
}
if (-not (Test-Path $Package)) { throw "package not found: $Package" }
$Package = (Resolve-Path $Package).Path

Write-Host "== checking $Board"
$ssh = @('-o', 'ConnectTimeout=8', '-o', 'StrictHostKeyChecking=accept-new', "root@$Board")
& ssh @ssh 'echo "   connected: $(tr -d "\0" < /proc/device-tree/model)"'
if ($LASTEXITCODE -ne 0) { throw "cannot reach root@$Board (USB cable: 172.32.0.93; network: look up the IP in your router; default password: luckfox)" }

Write-Host "== installing $Package"
# stream the package through ssh (no scp/sftp needed on the board); cmd handles the binary redirect
$remote = 'rm -rf /userdata/pcinst && mkdir -p /userdata/pcinst && gunzip -c | tar -x -C /userdata/pcinst'
cmd /c "ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new root@$Board `"$remote`" < `"$Package`""
if ($LASTEXITCODE -ne 0) { throw 'upload failed' }
& ssh @ssh 'sh /userdata/pcinst/install.sh; rc=$?; rm -rf /userdata/pcinst; exit $rc'
if ($LASTEXITCODE -ne 0) { throw 'install failed' }
Write-Host ''
Write-Host "PicoCam $version installed. Open http://$Board/ and set the admin password."
