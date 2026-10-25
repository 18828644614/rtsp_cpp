param(
    [Parameter(Mandatory = $true)][string]$ServerPath,
    [Parameter(Mandatory = $true)][string]$ClientPath,
    [Parameter(Mandatory = $true)][string]$WorkDirectory
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
$WorkDirectory = (Resolve-Path -LiteralPath $WorkDirectory).Path
$inputPath = Join-Path $WorkDirectory 'input.h264'
$prefix = [byte[]](0, 0, 0, 1)
$sample = [System.IO.MemoryStream]::new()
for ($frame = 0; $frame -lt 6; $frame++) {
    $sample.Write($prefix, 0, 4)
    $sample.Write([byte[]](0x09, 0xf0), 0, 2)
    if ($frame -eq 0) {
        $sample.Write($prefix, 0, 4)
        $sample.Write([byte[]](0x67, 0x64, 0, 0x1f), 0, 4)
        $sample.Write($prefix, 0, 4)
        $sample.Write([byte[]](0x68, 0xee), 0, 2)
    }
    $sample.Write($prefix, 0, 4)
    $sample.WriteByte(0x65)
    for ($index = 0; $index -lt 4000; $index++) { $sample.WriteByte(0x55 + $frame) }
}
$inputBytes = $sample.ToArray()
$sample.Dispose()
[System.IO.File]::WriteAllBytes($inputPath, $inputBytes)

function Check($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Start-App([string]$Path, [string[]]$Arguments, [string]$Name) {
    $quoted = $Arguments | ForEach-Object { '"' + $_ + '"' }
    return Start-Process -FilePath $Path -ArgumentList $quoted -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $WorkDirectory "$Name.out") -RedirectStandardError (Join-Path $WorkDirectory "$Name.err")
}

function Wait-App($Process, [string]$Name) {
    if (-not $Process.WaitForExit(10000)) { $Process.Kill(); throw "$Name timed out" }
    $Process.Refresh()
    $errorText = Get-Content -LiteralPath (Join-Path $WorkDirectory "$Name.err") -Raw
    Check ($Process.ExitCode -eq 0) "$Name exit=$($Process.ExitCode): $errorText"
}

function Stop-App($Process) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        $Process.Kill()
        $Process.WaitForExit()
    }
}

function Read-Bytes($Stream, [int]$Count) {
    $bytes = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $received = $Stream.Read($bytes, $offset, $Count - $offset)
        if ($received -eq 0) { throw 'unexpected EOF in test peer' }
        $offset += $received
    }
    return ,$bytes
}

$crlf = [string][char]13 + [char]10
function Read-Header($Stream) {
    $text = ''
    while (-not $text.EndsWith($crlf + $crlf)) {
        $value = $Stream.ReadByte()
        if ($value -lt 0) { throw 'unexpected EOF in RTSP header' }
        $text += [char]$value
        if ($text.Length -gt 16384) { throw 'test header too large' }
    }
    return $text
}

function Send-Text($Stream, [string]$Text) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text)
    $Stream.Write($bytes, 0, $bytes.Length)
}

function Request($Stream, [string]$Method, [int]$Cseq, [string]$Extra = '') {
    Send-Text $Stream "$Method rtsp://127.0.0.1/live RTSP/1.0$($crlf)CSeq: $Cseq$($crlf)$Extra$crlf"
    $header = Read-Header $Stream
    Check ($header.StartsWith('RTSP/1.0 200')) "request failed: $header"
    if ($header -match 'Content-Length: (\d+)') {
        $body = Read-Bytes $Stream ([int]$Matches[1])
        return [System.Text.Encoding]::ASCII.GetString($body)
    }
    return $header
}

function Start-TestServer([string]$Name) {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    $process = Start-App $ServerPath @("$port", '127.0.0.1', $inputPath) $Name
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $log = Get-Content -LiteralPath (Join-Path $WorkDirectory "$Name.out") -Raw -ErrorAction SilentlyContinue
        if ($log -match 'RTSP server listening') { return @($process, $port) }
        if ($process.HasExited) { throw "server failed to start: $Name" }
        Start-Sleep -Milliseconds 10
    }
    $process.Kill()
    throw 'server startup timed out'
}

function Make-Rtp([int]$Sequence, [byte[]]$Payload, [bool]$Marker = $true, [int]$Channel = 6, [int]$PayloadType = 104) {
    $markerBit = 0
    if ($Marker) { $markerBit = 128 }
    $length = 12 + $Payload.Length
    return ,([byte[]](36, $Channel, ($length -shr 8), ($length -band 255), 0x80, ($PayloadType -bor $markerBit), ($Sequence -shr 8), ($Sequence -band 255), 0, 0, 0x23, 0x28, 0, 0, 0, 1) + $Payload)
}

function Test-ClientReception([string]$Case) {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $output = Join-Path $WorkDirectory "$Case.h264"
    $process = $null
    $socket = $null
    try {
        $process = Start-App $ClientPath @('127.0.0.1', "$($listener.LocalEndpoint.Port)", $output) $Case
        $accept = $listener.AcceptTcpClientAsync()
        Check ($accept.Wait(5000)) 'mock accept timed out'
        $socket = $accept.Result
        $socket.ReceiveTimeout = 3000
        $stream = $socket.GetStream()
        for ($cseq = 1; $cseq -le 4; $cseq++) {
            $request = Read-Header $stream
            Check ($request -match "CSeq: $cseq$crlf") 'mock received incorrect CSeq'
            $headers = "Session: 12345$crlf"
            $body = ''
            if ($cseq -eq 2) {
                $body = "v=0$($crlf)a=control:*$($crlf)m=video 0 RTP/AVP 104$($crlf)a=rtpmap:104 H264/90000$($crlf)a=fmtp:104 packetization-mode=1$($crlf)a=control:trackID=0$crlf"
                $headers = "Content-Type: application/sdp$($crlf)Content-Base: rtsp://127.0.0.1/live/$($crlf)Content-Length: $($body.Length)$crlf"
            }
            if ($cseq -eq 3) { $headers += "Transport: RTP/AVP/TCP;unicast;interleaved=6-7$crlf" }
            $response = "RTSP/1.0 200 OK$($crlf)CSeq: $cseq$($crlf)$headers$crlf$body"
            if ($cseq -lt 4) { Send-Text $stream $response; continue }
            $media = Make-Rtp 65535 ([byte[]](0x65, 0xaa))
            if ($Case -eq 'mock-valid') {
                $media = [byte[]](36, 7, 0, 1, 0) + $media + (Make-Rtp 0 ([byte[]](0x41, 0xbb)))
            } elseif ($Case -eq 'mock-truncated') {
                $media = $media[0..($media.Length - 2)]
            } elseif ($Case -eq 'mock-fu-eof') {
                $media = Make-Rtp 65535 ([byte[]](0x7c, 0x85, 0xaa)) $false
            } elseif ($Case -eq 'mock-no-marker') {
                $media = Make-Rtp 65535 ([byte[]](0x65, 0xaa)) $false
            } elseif ($Case -eq 'mock-wrong-pt') {
                $media = Make-Rtp 65535 ([byte[]](0x65, 0xaa)) $true 6 105
            } elseif ($Case -eq 'mock-wrong-channel') {
                $media = Make-Rtp 65535 ([byte[]](0x65, 0xaa)) $true 8
            } elseif ($Case -eq 'mock-gap') {
                $media += Make-Rtp 1 ([byte[]](0x41, 0xbb))
            }
            $wire = [System.Text.Encoding]::ASCII.GetBytes($response) + $media
            $stream.Write($wire, 0, 7)
            $stream.Write($wire, 7, $wire.Length - 7)
            $socket.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        }
        Check ($process.WaitForExit(5000)) 'mock client timed out'
        $process.Refresh()
        if ($Case -eq 'mock-valid') {
            Check ($process.ExitCode -eq 0) 'client lost media adjacent to PLAY response'
            $expected = [byte[]](0, 0, 0, 1, 0x65, 0xaa, 0, 0, 0, 1, 0x41, 0xbb)
            Check ([Convert]::ToBase64String([System.IO.File]::ReadAllBytes($output)) -eq [Convert]::ToBase64String($expected)) 'mock media content changed'
        } else {
            Check ($process.ExitCode -eq 1) "$Case was silently accepted"
        }
    } finally {
        if ($null -ne $socket) { $socket.Dispose() }
        $listener.Stop()
        if ($null -ne $process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    }
}

$server = $null
$client = $null
$clientA = $null
$clientB = $null
$clientC = $null
$peer = $null
try {
    $started = Start-TestServer 'server-full'
    $server = $started[0]
    $outputA = Join-Path $WorkDirectory 'received-a.h264'
    $outputB = Join-Path $WorkDirectory 'received-b.h264'
    $clientA = Start-App $ClientPath @('127.0.0.1', "$($started[1])", $outputA) 'client-a'
    $clientB = Start-App $ClientPath @('127.0.0.1', "$($started[1])", $outputB) 'client-b'
    Wait-App $clientA 'client-a'
    Wait-App $clientB 'client-b'
    foreach ($outputPath in @($outputA, $outputB)) {
        $receivedBytes = [System.IO.File]::ReadAllBytes($outputPath)
        Check ([Convert]::ToBase64String($receivedBytes) -eq [Convert]::ToBase64String($inputBytes)) "multi-client transfer changed $outputPath"
    }
    $sessionMatches = [regex]::Matches((Get-Content -LiteralPath (Join-Path $WorkDirectory 'server-full.out') -Raw), 'Client connected, Session=(\d+)')
    $sessionIds = @($sessionMatches | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    Check ($sessionIds.Count -ge 2) 'multi-client server reused or failed to report Session IDs'

    $outputC = Join-Path $WorkDirectory 'received-c.h264'
    $clientC = Start-App $ClientPath @('127.0.0.1', "$($started[1])", $outputC) 'client-reconnect'
    Wait-App $clientC 'client-reconnect'
    $receivedBytes = [System.IO.File]::ReadAllBytes($outputC)
    Check ([Convert]::ToBase64String($receivedBytes) -eq [Convert]::ToBase64String($inputBytes)) 'server did not accept a client after disconnects'
    Stop-App $server

    $started = Start-TestServer 'server-stop'
    $server = $started[0]
    $partialPath = Join-Path $WorkDirectory 'partial.h264'
    $client = Start-App $ClientPath @('127.0.0.1', "$($started[1])", $partialPath, '2') 'client-stop'
    Wait-App $client 'client-stop'
    Stop-App $server
    Check ((Get-Item -LiteralPath $partialPath).Length -lt $inputBytes.Length) 'TEARDOWN did not stop media'
    Check ((Get-Content -LiteralPath (Join-Path $WorkDirectory 'server-stop.out') -Raw) -match 'Request: TEARDOWN') 'server did not process TEARDOWN'

    $started = Start-TestServer 'server-wire'
    $server = $started[0]
    $peer = [System.Net.Sockets.TcpClient]::new('127.0.0.1', [int]$started[1])
    $peer.ReceiveTimeout = 3000
    $stream = $peer.GetStream()
    $sdp = Request $stream 'DESCRIBE' 1
    Check ($sdp -match 'sprop-parameter-sets=Z2QAHw==,aO4=') 'SDP did not contain file parameters'
    Send-Text $stream "SETUP rtsp://127.0.0.1/live/trackID=0 RTSP/1.0$($crlf)CSeq: 2$($crlf)Transport: RTP/AVP/TCP;unicast;interleaved=6-7$crlf$crlf"
    $setup = Read-Header $stream
    Check ($setup -match 'Session: (\d+)') 'SETUP omitted Session'
    $session = $Matches[1]
    $play = Request $stream 'PLAY' 3 "Session: $session$crlf"
    $sequence = 100
    $frameCount = 0
    $packetCount = 0
    $fragmentCount = 0
    while ($true) {
        $first = $stream.ReadByte()
        if ($first -lt 0) { break }
        Check ($first -eq 36) 'expected an interleaved frame'
        $header = Read-Bytes $stream 3
        Check ($header[0] -eq 6) 'server ignored negotiated RTP channel'
        $rtp = Read-Bytes $stream ($header[1] * 256 + $header[2])
        Check ($rtp[0] -eq 0x80 -and ($rtp[1] -band 127) -eq 96) 'invalid RTP version or PT'
        Check (($rtp[2] * 256 + $rtp[3]) -eq ($sequence -band 65535)) 'RTP sequence gap'
        $timestamp = [uint32]$rtp[4] * 16777216 + [uint32]$rtp[5] * 65536 + $rtp[6] * 256 + $rtp[7]
        Check ($timestamp -eq $frameCount * 3600) 'timestamp changed within an access unit'
        if (($rtp[12] -band 31) -eq 28) { $fragmentCount++ }
        if (($rtp[1] -band 128) -ne 0) { $frameCount++ }
        $sequence++
        $packetCount++
    }
    Check ($frameCount -eq 6 -and $packetCount -gt 6 -and $fragmentCount -gt 0) 'marker or FU-A generation failed'
    $peer.Dispose()
    $peer = $null
    Stop-App $server
    foreach ($case in @('mock-valid', 'mock-truncated', 'mock-fu-eof', 'mock-no-marker', 'mock-wrong-pt', 'mock-wrong-channel', 'mock-gap')) {
        Test-ClientReception $case
    }
    Write-Output 'PASS: full transfer, TEARDOWN, SDP, channels, RTP fields and FU-A'
} finally {
    if ($null -ne $peer) { $peer.Dispose() }
    foreach ($process in @($client, $clientA, $clientB, $clientC, $server)) {
        Stop-App $process
    }
}
