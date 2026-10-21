<#
Starts MediaMTX (live video relay) and edgeai-server (API + dashboard) as detached processes
(they keep running after this window closes). Stop them with .\stop.ps1.
Dashboard: http://<this-pc-ip>:8090
Windows may ask to allow both through the firewall: allow on Private networks
(boards post to :8090; browsers use :8889 TCP and :8189 UDP for WebRTC video).
#>
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Get-Process mediamtx, edgeai-server -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

function Start-Detached($cmd, $dir) {
    # WMI-created processes are not tied to this console/session
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $dir }
    if ($r.ReturnValue -ne 0) { throw "failed to start: $cmd" }
}

Start-Detached "cmd /c `"`"$here\mediamtx\mediamtx.exe`" `"$here\mediamtx\mediamtx.yml`" > `"$here\mediamtx.log`" 2>&1`"" "$here\mediamtx"
Start-Sleep -Seconds 1
Start-Detached "cmd /c `"`"$here\edgeai-server.exe`" -addr :8090 -data `"$here\data`" -web `"$here\..\web\dist`" > `"$here\server.log`" 2> `"$here\server.err.log`"`"" $here
Start-Sleep -Seconds 2
Get-Process mediamtx, edgeai-server -ErrorAction SilentlyContinue | Format-Table Name, Id -AutoSize
Write-Host "EdgeAI started. Dashboard: http://localhost:8090  (logs: server.err.log, mediamtx.log)"
