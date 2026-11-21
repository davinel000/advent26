[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('play', 'stop', 'status', 'schedule', 'clear', 'advent-on', 'advent-off', 'upload', 'restore')]
    [string]$Operation = 'play',

    [Parameter(Position = 1)]
    [string]$At,

    [string]$Melody,

    [ValidateSet('dry', 'echo')]
    [string]$Effect,

    [string]$NotesPath,

    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
$operationName = $Operation.ToLowerInvariant()
if ($operationName -notin @('play', 'schedule', 'upload', 'restore') -and $PSBoundParameters.ContainsKey('Melody')) {
    throw 'Use -Melody with play, schedule, upload or restore.'
}
if ($operationName -notin @('play', 'schedule') -and $PSBoundParameters.ContainsKey('Effect')) {
    throw 'Use -Effect with play or schedule.'
}
if ($operationName -ne 'upload' -and $PSBoundParameters.ContainsKey('NotesPath')) {
    throw 'Use -NotesPath with upload.'
}
$melodyId = if ($Melody) { ($Melody.Trim().ToLowerInvariant() -replace '\s+', '-') } else { 'jingle1' }
if ($melodyId -notmatch '^[a-z0-9][a-z0-9-]{0,39}$' -or
    ($melodyId -match '^day\d+$' -and $melodyId -notmatch '^day(0[1-9]|1[0-9]|2[0-4])$')) {
    throw 'Use a melody name such as a-forest or a slot from day01 through day24.'
}

function New-Command([string]$Op) {
    $time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    return [ordered]@{
        v = 1
        id = "$Op-$time"
        op = $Op
        issued_at = $time
        expires_at = $time + 60
    }
}

$op = if ($operationName -in @('advent-on', 'advent-off')) { 'advent' } else { $operationName }
$command = New-Command $op
if ($op -eq 'advent') {
    $command['enabled'] = $operationName -eq 'advent-on'
}
if ($operationName -eq 'schedule') {
    $target = [DateTimeOffset]::MinValue
    $formats = [string[]]@("yyyy-MM-dd'T'HH:mm:sszzz", "yyyy-MM-dd'T'HH:mm:ss'Z'")
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal
    if (-not [DateTimeOffset]::TryParseExact($At, $formats, $culture, $styles, [ref]$target) -or
        $target.ToUnixTimeSeconds() -le $command.issued_at) {
        throw 'Use a future ISO timestamp with seconds and a UTC offset or Z.'
    }
    $command['fire_at_utc'] = $target.ToUnixTimeSeconds()
} elseif ($PSBoundParameters.ContainsKey('At')) {
    throw 'Use -At with the schedule operation.'
}

if ($operationName -in @('play', 'schedule')) {
    $command['melody'] = $melodyId
}
if ($Effect) {
    $command['effect'] = $Effect.ToLowerInvariant()
}
if ($operationName -eq 'upload') {
    if (!$NotesPath -or !$Melody -or $melodyId -eq 'jingle1') {
        throw 'Specify -NotesPath and a melody name or day01 through day24.'
    }
    $notes = Get-Content -Raw -Encoding utf8 -LiteralPath $NotesPath | ConvertFrom-Json -NoEnumerate
    if ($notes -isnot [array] -or !$notes.Count -or $notes.Count -gt 512) {
        throw 'The JSON file must contain between 1 and 512 [Hz, milliseconds] pairs.'
    }
    $stream = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($stream)
    $duration = 0
    $hasPitch = $false
    try {
        foreach ($pair in $notes) {
            if ($pair -isnot [array] -or $pair.Count -ne 2 -or
                $pair[0] -isnot [long] -or $pair[1] -isnot [long] -or
                ($pair[0] -ne 0 -and ($pair[0] -lt 80 -or $pair[0] -gt 2500)) -or
                $pair[1] -lt 1 -or $pair[1] -gt 10000) {
                throw 'Each pair needs integer Hz (0 or 80–2500) and 1–10000 milliseconds.'
            }
            $writer.Write([uint16]$pair[0])
            $writer.Write([uint16]$pair[1])
            $hasPitch = $hasPitch -or $pair[0] -ne 0
            $duration += $pair[1]
        }
        if ($notes[-1][0] -ne 0 -or $notes[-1][1] -lt 60) {
            $writer.Write([uint16]0)
            $writer.Write([uint16]60)
            $duration += 60
        }
        $rawNotes = $stream.ToArray()
    } finally {
        $writer.Dispose()
    }
    if (!$hasPitch -or $duration -gt 60000 -or $rawNotes.Length -gt 2048) {
        throw 'The melody needs at least one tone, at most 512 events and at most 60 seconds including its final rest.'
    }
}
if ($operationName -eq 'restore' -and (!$Melody -or $melodyId -eq 'jingle1')) {
    throw 'Specify the melody or slot to restore.'
}

if ($Preview) {
    if ($operationName -eq 'upload') {
        @{melody = $melodyId; bytes = $rawNotes.Length; duration_ms = $duration} | ConvertTo-Json -Compress
    } elseif ($operationName -eq 'restore') {
        @{melody = $melodyId; part = 'restore'} | ConvertTo-Json -Compress
    } else {
        @{value = ($command | ConvertTo-Json -Compress)} | ConvertTo-Json -Compress
    }
    return
}

$header = Join-Path $env:LOCALAPPDATA 'Advent26/secrets.txt'
if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
    $header = Join-Path $PSScriptRoot '../firmware/music_box/secrets.h'
}
if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
    throw 'Prepare a local secrets.txt or fill in firmware/music_box/secrets.h before sending a command.'
}
$settings = [IO.File]::ReadAllText($header)
$account = [regex]::Match($settings, '(?m)^[\t ]*#define[\t ]+ADAFRUIT_IO_USERNAME[\t ]+"([a-zA-Z0-9_-]+)"').Groups[1].Value
$key = [regex]::Match($settings, '(?m)^[\t ]*#define[\t ]+ADAFRUIT_IO_KEY[\t ]+"([a-zA-Z0-9_-]+)"').Groups[1].Value
if (-not $account -or -not $key) {
    throw 'Fill in ADAFRUIT_IO_USERNAME and ADAFRUIT_IO_KEY in the local credentials file.'
}
$uri = "https://io.adafruit.com/api/v2/$account/feeds/musicbox-command/data"
function Send-CommandValue($Request) {
    $value = $Request | ConvertTo-Json -Compress
    if ([Text.Encoding]::UTF8.GetByteCount($value) -gt 240) {
        throw 'Command exceeds the device packet budget.'
    }
    $body = @{value = $value} | ConvertTo-Json -Compress
    try {
        $null = Invoke-RestMethod -Method Post -Uri $uri -ContentType 'application/json' -Body $body `
        -Headers @{ 'X-AIO-Key' = $key } -TimeoutSec 12 -MaximumRedirection 0 -Verbose:$false -Debug:$false
    } catch {
        $response = $_.Exception.PSObject.Properties['Response']
        if ($response -and $response.Value.StatusCode) {
            throw "Adafruit IO returned HTTP $([int]$response.Value.StatusCode)."
        }
        throw 'The request could not be confirmed. Check musicbox-status before trying again.'
    }
}

if ($operationName -notin @('upload', 'restore')) {
    Send-CommandValue $command
    Write-Output "Published $($command.id). Check musicbox-status for the device response."
    return
}

$transfer = Get-Random -Minimum 1 -Maximum ([int]::MaxValue)
$statusUri = "https://io.adafruit.com/api/v2/$account/feeds/musicbox-status/data?limit=8"
function Send-NotesPart([string]$Part, [hashtable]$Extra) {
    $request = New-Command 'notes'
    $request['melody'] = $melodyId
    $request['part'] = $Part
    $request['transfer'] = $transfer
    foreach ($name in $Extra.Keys) { $request[$name] = $Extra[$name] }
    Send-CommandValue $request
    for ($attempt = 0; $attempt -lt 8; $attempt++) {
        Start-Sleep -Seconds 2
        try {
            $data = Invoke-RestMethod -Uri $statusUri -Headers @{'X-AIO-Key' = $key} `
                -TimeoutSec 8 -MaximumRedirection 0 -Verbose:$false -ErrorAction Stop
        } catch { continue }
        foreach ($row in @($data)) {
            if (!$row.value) { continue }
            $ack = $row.value | ConvertFrom-Json -ErrorAction SilentlyContinue
            if ($ack.id -eq $request.id) {
                if ($ack.state -ne 'accepted') { throw "ESP32 rejected $Part ($($ack.error))." }
                return
            }
        }
    }
    throw "ESP32 did not confirm $Part. An incomplete upload does not replace the melody."
}

if ($operationName -eq 'restore') {
    Send-NotesPart 'restore' @{}
    Write-Output "ESP32 restored the built-in melody for $melodyId."
    return
}

$largestPacket = New-Command 'notes'
$largestPacket['melody'] = $melodyId
$largestPacket['part'] = 'append'
$largestPacket['transfer'] = $transfer
$largestPacket['offset'] = $rawNotes.Length
$largestPacket['data'] = ''
$envelopeBytes = [Text.Encoding]::UTF8.GetByteCount(($largestPacket | ConvertTo-Json -Compress))
$chunkBytes = [int][Math]::Min(54, 3 * [Math]::Floor((240 - $envelopeBytes) / 4))
Send-NotesPart 'begin' @{size = $rawNotes.Length}
for ($offset = 0; $offset -lt $rawNotes.Length; $offset += $chunkBytes) {
    $count = [Math]::Min($chunkBytes, $rawNotes.Length - $offset)
    Send-NotesPart 'append' @{offset = $offset; data = [Convert]::ToBase64String($rawNotes, $offset, $count)}
}
Send-NotesPart 'commit' @{}
Write-Output "ESP32 saved the replacement for $melodyId."
