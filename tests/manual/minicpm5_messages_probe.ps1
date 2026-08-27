# Opt-in live probe for a vLLM Anthropic Messages endpoint serving MiniCPM5-1B.
# It never enters CTest. The report stores counts and event kinds only: no API
# key, request id, thinking text, signature, image bytes, or tool result body.

[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://localhost:8001',
    [string]$Model = 'MiniCPM5-1B',
    [string]$ApiKey = 'unused',
    [ValidateRange(1, 10)]
    [int]$Repeats = 3,
    [string]$ReportDir = 'build/test-evidence/minicpm5-messages-probe'
)

$ErrorActionPreference = 'Stop'
$BaseUrl = $BaseUrl.TrimEnd('/')
$headers = @{
    'x-api-key' = $ApiKey
    'anthropic-version' = '2023-06-01'
    'content-type' = 'application/json'
}

function Convert-ResponseText {
    param($Content)
    if ($Content -is [byte[]]) {
        return [Text.Encoding]::UTF8.GetString($Content)
    }
    return [string]$Content
}

function Invoke-JsonPost {
    param(
        [string]$Path,
        [hashtable]$Body
    )
    $response = Invoke-WebRequest -SkipHttpErrorCheck `
        -Uri ($BaseUrl + $Path) `
        -Method Post `
        -Headers $headers `
        -Body ($Body | ConvertTo-Json -Depth 40 -Compress) `
        -TimeoutSec 120
    $text = Convert-ResponseText $response.Content
    $json = $null
    if (-not [string]::IsNullOrWhiteSpace($text)) {
        try { $json = $text | ConvertFrom-Json -Depth 40 } catch { }
    }
    return [pscustomobject]@{
        Status = [int]$response.StatusCode
        Text = $text
        Json = $json
    }
}

function Measure-Blocks {
    param($ResponseJson)
    $thinking = (@($ResponseJson.content | Where-Object type -eq 'thinking' |
        ForEach-Object thinking) -join '')
    $text = (@($ResponseJson.content | Where-Object type -eq 'text' |
        ForEach-Object text) -join '')
    return [pscustomobject]@{
        Blocks = (@($ResponseJson.content.type) -join ',')
        ThinkingChars = $thinking.Length
        TextChars = $text.Length
        StopReason = [string]$ResponseJson.stop_reason
        OutputTokens = [int64]$ResponseJson.usage.output_tokens
    }
}

function Read-SseSummary {
    param([string]$Raw)
    $eventTypes = @()
    $blockTypes = @()
    $deltaTypes = @()
    $text = ''
    $thinking = ''
    $stopReason = ''
    foreach ($line in ($Raw -split "\r?\n")) {
        if (-not $line.StartsWith('data: ')) { continue }
        $data = $line.Substring(6)
        if ($data -eq '[DONE]') { continue }
        try { $event = $data | ConvertFrom-Json -Depth 40 } catch { continue }
        if ($event.type) { $eventTypes += [string]$event.type }
        if ($event.type -eq 'content_block_start') {
            $blockTypes += [string]$event.content_block.type
        }
        if ($event.type -eq 'content_block_delta') {
            $deltaTypes += [string]$event.delta.type
            if ($event.delta.type -eq 'text_delta') { $text += [string]$event.delta.text }
            if ($event.delta.type -eq 'thinking_delta') { $thinking += [string]$event.delta.thinking }
        }
        if ($event.type -eq 'message_delta') {
            $stopReason = [string]$event.delta.stop_reason
        }
    }
    return [pscustomobject]@{
        EventTypes = (($eventTypes | Select-Object -Unique) -join ',')
        BlockTypes = (($blockTypes | Select-Object -Unique) -join ',')
        DeltaTypes = (($deltaTypes | Select-Object -Unique) -join ',')
        StopReason = $stopReason
        TextChars = $text.Length
        ThinkingChars = $thinking.Length
        TextHasOpenThink = $text.Contains('<think>')
        TextHasCloseThink = $text.Contains('</think>')
    }
}

try {
    $modelsResponse = Invoke-WebRequest -SkipHttpErrorCheck `
        -Uri ($BaseUrl + '/v1/models') -Headers @{'x-api-key' = $ApiKey} -TimeoutSec 30
} catch {
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
    $failurePath = Join-Path $ReportDir 'report.json'
    [ordered]@{
        captured_at = (Get-Date).ToString('o')
        base_url = $BaseUrl
        model = $Model
        probe_error = 'endpoint_unavailable'
        phase = 'models'
        detail = 'GET /v1/models timed out or could not connect'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $failurePath -Encoding utf8
    [Console]::Error.WriteLine("GET /v1/models did not respond; failure report: $failurePath")
    exit 2
}
if ([int]$modelsResponse.StatusCode -ne 200) {
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
    $failurePath = Join-Path $ReportDir 'report.json'
    [ordered]@{
        captured_at = (Get-Date).ToString('o')
        base_url = $BaseUrl
        model = $Model
        probe_error = 'endpoint_unavailable'
        phase = 'models'
        status = [int]$modelsResponse.StatusCode
        detail = 'GET /v1/models returned a non-200 status'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $failurePath -Encoding utf8
    [Console]::Error.WriteLine(
        "GET /v1/models returned HTTP $([int]$modelsResponse.StatusCode); failure report: $failurePath")
    exit 2
}
$modelsJson = (Convert-ResponseText $modelsResponse.Content) | ConvertFrom-Json -Depth 20
$modelEntry = @($modelsJson.data | Where-Object id -eq $Model)[0]
if ($null -eq $modelEntry) { throw "Model '$Model' is absent from GET /v1/models" }

$dialectRows = @()
foreach ($mode in @('none', 'high')) {
    foreach ($run in 1..$Repeats) {
        $thinking = if ($mode -eq 'none') {
            @{type = 'disabled'}
        } else {
            @{type = 'enabled'; budget_tokens = 256}
        }
        $response = Invoke-JsonPost '/v1/messages' @{
            model = $Model
            max_tokens = 1024
            thinking = $thinking
            messages = @(@{role = 'user'; content = 'Reply only CONTROL_OK.'})
        }
        $measure = Measure-Blocks $response.Json
        $dialectRows += [pscustomobject]@{
            mode = $mode
            run = $run
            status = $response.Status
            blocks = $measure.Blocks
            thinking_chars = $measure.ThinkingChars
            text_chars = $measure.TextChars
            stop_reason = $measure.StopReason
            output_tokens = $measure.OutputTokens
        }
    }
}

$templateRows = @()
foreach ($enabled in @($false, $true)) {
    foreach ($run in 1..$Repeats) {
        $response = Invoke-JsonPost '/v1/messages' @{
            model = $Model
            max_tokens = 1024
            chat_template_kwargs = @{enable_thinking = $enabled}
            messages = @(@{role = 'user'; content = 'Reply only TEMPLATE_OK.'})
        }
        $measure = Measure-Blocks $response.Json
        $templateRows += [pscustomobject]@{
            enable_thinking = $enabled
            run = $run
            status = $response.Status
            blocks = $measure.Blocks
            thinking_chars = $measure.ThinkingChars
            text_chars = $measure.TextChars
            stop_reason = $measure.StopReason
            output_tokens = $measure.OutputTokens
        }
    }
}

$tool = @{
    name = 'probe_file'
    description = 'Return fixed probe text'
    input_schema = @{
        type = 'object'
        properties = @{ignore = @{type = 'string'}}
    }
}
$toolPrompt = 'Call probe_file once. After its result, reply only DONE.'
$toolSummary = [pscustomobject]@{attempted = $true; tool_use = $false}
foreach ($attempt in 1..3) {
    $first = Invoke-JsonPost '/v1/messages' @{
        model = $Model
        max_tokens = 512
        thinking = @{type = 'enabled'; budget_tokens = 256}
        tools = @($tool)
        tool_choice = @{type = 'tool'; name = 'probe_file'}
        messages = @(@{role = 'user'; content = $toolPrompt})
    }
    $toolUse = @($first.Json.content | Where-Object type -eq 'tool_use')[0]
    if ($null -eq $toolUse) { continue }

    $second = Invoke-JsonPost '/v1/messages' @{
        model = $Model
        max_tokens = 512
        thinking = @{type = 'enabled'; budget_tokens = 256}
        tools = @($tool)
        stream = $true
        messages = @(
            @{role = 'user'; content = $toolPrompt},
            @{role = 'assistant'; content = @($first.Json.content)},
            @{role = 'user'; content = @(@{
                type = 'tool_result'
                tool_use_id = $toolUse.id
                content = ('P' * 2400)
            })}
        )
    }
    $sse = Read-SseSummary $second.Text
    $toolSummary = [pscustomobject]@{
        attempted = $true
        attempt = $attempt
        tool_use = $true
        first_status = $first.Status
        first_stop_reason = [string]$first.Json.stop_reason
        second_status = $second.Status
        event_types = $sse.EventTypes
        block_types = $sse.BlockTypes
        delta_types = $sse.DeltaTypes
        second_stop_reason = $sse.StopReason
        text_chars = $sse.TextChars
        thinking_chars = $sse.ThinkingChars
        text_has_open_think = $sse.TextHasOpenThink
        text_has_close_think = $sse.TextHasCloseThink
    }
    break
}

$oversizeText = 'a ' * 40000
$countResponse = Invoke-JsonPost '/v1/messages/count_tokens' @{
    model = $Model
    messages = @(@{role = 'user'; content = $oversizeText})
}
$imageResponse = Invoke-JsonPost '/v1/messages' @{
    model = $Model
    max_tokens = 16
    messages = @(@{
        role = 'user'
        content = @(
            @{
                type = 'image'
                source = @{
                    type = 'base64'
                    media_type = 'image/png'
                    data = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII='
                }
            },
            @{type = 'text'; text = 'Describe the image.'}
        )
    })
}

$findings = @()
if (@($dialectRows | Where-Object {$_.mode -eq 'none' -and $_.thinking_chars -gt 0}).Count -gt 0) {
    $findings += 'thinking_disabled_ignored'
}
# 预算耗尽只判 inconclusive(MiniCPM5 巡检单 P1):stop=max_tokens 且正文 0
# 的回没有"正文是否产出"的发言权,不得当支持或不支持的证据。
foreach ($mode in @('none', 'high')) {
    $modeRows = @($dialectRows | Where-Object {$_.mode -eq $mode})
    $exhausted = @($modeRows | Where-Object {$_.stop_reason -eq 'max_tokens' -and $_.text_chars -eq 0})
    if ($modeRows.Count -gt 0 -and $exhausted.Count -eq $modeRows.Count) {
        $findings += "probe_budget_exhausted_$mode`_inconclusive"
    }
}
if (@($templateRows | Where-Object {$_.enable_thinking -eq $false -and $_.text_chars -eq 0}).Count -eq $Repeats) {
    $findings += 'template_off_returns_no_text'
}
if ($toolSummary.tool_use -and $toolSummary.text_has_open_think) {
    $findings += 'post_tool_raw_think_in_text_delta'
}
if ($countResponse.Status -ge 500) { $findings += 'count_tokens_overflow_is_5xx' }
if ($imageResponse.Status -ge 500) { $findings += 'unsupported_image_is_5xx' }

$report = [ordered]@{
    captured_at = (Get-Date).ToString('o')
    base_url = $BaseUrl
    model = $Model
    model_metadata = [ordered]@{
        owned_by = [string]$modelEntry.owned_by
        max_model_len = [int64]$modelEntry.max_model_len
    }
    repeats = $Repeats
    dialect = $dialectRows
    template_toggle = $templateRows
    tool_roundtrip = $toolSummary
    oversize_count_tokens = [ordered]@{
        status = $countResponse.Status
        error_type = [string]$countResponse.Json.error.type
    }
    image_capability = [ordered]@{
        status = $imageResponse.Status
        error_type = [string]$imageResponse.Json.error.type
    }
    findings = $findings
}

New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
$reportPath = Join-Path $ReportDir 'report.json'
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding utf8
$report | ConvertTo-Json -Depth 20
Write-Host "[minicpm5-probe] report: $reportPath"
