[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('play', 'stop', 'status', 'schedule', 'clear')]
    [string]$Operation = 'play',

    [Parameter(Position = 1)]
    [string]$At,

    [ValidatePattern('^(jingle1|day(?:0[1-9]|1[0-9]|2[0-4]))$')]
    [string]$Melody,

    [ValidateSet('dry', 'echo')]
    [string]$Effect,

    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
$operationName = $Operation.ToLowerInvariant()
if ($operationName -ne 'play' -and
    ($PSBoundParameters.ContainsKey('Melody') -or $PSBoundParameters.ContainsKey('Effect'))) {
    throw 'Use -Melody and -Effect with the play operation.'
}
$sent = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$command = [ordered]@{
    v = 1
    id = "$operationName-$sent"
    op = $operationName
    issued_at = $sent
    expires_at = $sent + 60
}

if ($operationName -eq 'schedule') {
    $target = [DateTimeOffset]::MinValue
    $formats = [string[]]@("yyyy-MM-dd'T'HH:mm:sszzz", "yyyy-MM-dd'T'HH:mm:ss'Z'")
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal
    if (-not [DateTimeOffset]::TryParseExact($At, $formats, $culture, $styles, [ref]$target) -or
        $target.ToUnixTimeSeconds() -le $sent) {
        throw 'Use a future ISO timestamp with seconds and a UTC offset or Z.'
    }
    $command['fire_at_utc'] = $target.ToUnixTimeSeconds()
} elseif ($PSBoundParameters.ContainsKey('At')) {
    throw 'Use -At with the schedule operation.'
}

if ($operationName -in @('play', 'schedule')) {
    $command['melody'] = if ($Melody) { $Melody.ToLowerInvariant() } else { 'jingle1' }
}
if ($Effect) {
    $command['effect'] = $Effect.ToLowerInvariant()
}
$value = $command | ConvertTo-Json -Compress
$body = @{ value = $value } | ConvertTo-Json -Compress

if ($Preview) {
    $body
    return
}

$header = Join-Path $PSScriptRoot '../firmware/music_box/secrets.h'
if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
    throw 'Fill in firmware/music_box/secrets.h before sending a command.'
}
$settings = [IO.File]::ReadAllText($header)
$account = [regex]::Match($settings, '(?m)^[\t ]*#define[\t ]+ADAFRUIT_IO_USERNAME[\t ]+"([a-zA-Z0-9_-]+)"').Groups[1].Value
$key = [regex]::Match($settings, '(?m)^[\t ]*#define[\t ]+ADAFRUIT_IO_KEY[\t ]+"([a-zA-Z0-9_-]+)"').Groups[1].Value
if (-not $account -or -not $key) {
    throw 'Fill in ADAFRUIT_IO_USERNAME and ADAFRUIT_IO_KEY in secrets.h.'
}
$uri = "https://io.adafruit.com/api/v2/$account/feeds/musicbox-command/data"
try {
    $null = Invoke-RestMethod -Method Post -Uri $uri -ContentType 'application/json' -Body $body `
        -Headers @{ 'X-AIO-Key' = $key } -TimeoutSec 12 -MaximumRedirection 0 -Verbose:$false -Debug:$false
} catch {
    $responseProperty = $_.Exception.PSObject.Properties['Response']
    $response = if ($responseProperty) { $responseProperty.Value }
    $code = if ($response -and $response.StatusCode) { [int]$response.StatusCode }
    if ($code) {
        throw "Adafruit IO returned HTTP $code."
    }
    throw 'The request could not be confirmed. Check musicbox-status before trying again.'
}

Write-Output "Published $($command.id). Check musicbox-status for the device response."
