# 遥测与本地 Collector

设计全账见 `todos/端云协同可观测架构与Telemetry插件设计.todo`。本文只讲怎么把
LubanCode 的 OTLP 出口接到一台本机 Collector 上看数据。默认关闭:不开
`features.telemetry`,零线程、零目录、零出站。

## 配置(LubanCode 侧)

`~/.lubancode/config.json`:

```json
{
  "features": {
    "trajectory": true,
    "telemetry": true
  },
  "telemetry": {
    "data_class": "metadata",
    "exporter": {
      "kind": "otlp-http-json",
      "endpoint": "http://127.0.0.1:4318",
      "secret_ref": null,
      "compression": "none",
      "timeout_ms": 10000
    },
    "queue": {
      "capacity_items": 8192,
      "capacity_bytes": 16777216
    },
    "spool": {
      "enabled": true,
      "max_bytes": 268435456,
      "max_age_hours": 48
    }
  }
}
```

要点:

- `features.trajectory` 必须同开(遥测的事实源是 Trajectory Journal)。
- `endpoint` 指 Collector 的 OTLP/HTTP 接收口(`4318`),不是后端地址。
- 回环地址(127.0.0.1/localhost/[::1])免公网披露问答;公网 endpoint 必须
  HTTPS,且先敲 `/telemetry consent grant` 授权才发。
- `secret_ref` 填环境变量名(不是值):Collector 要 Bearer 认证时才需要。
- `compression` 首版只认 `none`(构建未启用 zlib)。
- `sampling` 与 `remote_policy` 属后续批次,写了会报错。

## 本地 Collector(docker)

`otel-collector-config.yaml`(全部出口留在本机文件与控制台,不出网):

```yaml
receivers:
  otlp:
    protocols:
      http:
        endpoint: 127.0.0.1:4318

exporters:
  # 控制台看一眼(调试用);要接真后端时换 otlphttp/otlp 指你的后端
  debug:
    verbosity: basic
  file:
    path: ./lubancode-traces.json

service:
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [debug, file]
    metrics:
      receivers: [otlp]
      exporters: [debug]
```

`docker-compose.yml`:

```yaml
services:
  otel-collector:
    image: otel/opentelemetry-collector:latest
    network_mode: host          # Linux 直用 127.0.0.1:4318
    volumes:
      - ./otel-collector-config.yaml:/etc/otelcol/config.yaml:ro
```

Windows/macOS 上 `network_mode: host` 不可用时,把 compose 的
`4318:4318` 端口映射出来,endpoint 仍写 `http://127.0.0.1:4318`。

## 不用 docker(本地二进制)

从 OpenTelemetry 项目下载 collector 发行包,同一份配置起:

```sh
otelcol --config otel-collector-config.yaml
```

## 命令速查

| 命令 | 作用 |
| --- | --- |
| `/telemetry` | 本地状态,不发请求 |
| `/telemetry enable session` | 只当前进程开(不写配置) |
| `/telemetry enable config` | 写全局配置(项目配置不动) |
| `/telemetry pause` / `resume` | 停/复出口,投影照常落 spool |
| `/telemetry flush [毫秒]` | seal 后有界赶发,不吊死在公网 |
| `/telemetry spool` / `spool clear --confirm` | 看账/两步确认删除 |
| `/telemetry consent grant` / `revoke` | 公网发送授权(回环不需要) |
| `/doctor telemetry` / `--probe` | 本地诊断 / 对 endpoint 发无业务数据探针 |

## 可靠性行为

at-least-once:断网、Collector 重启、429/5xx 都按指数退避补送(尊重
`Retry-After`,受本地帽);404/401 等永久错停该 endpoint 一代并报
`/doctor telemetry`;批次带 `x-lubancode-batch-id` 头,对端可凭它去重
(响应丢失后补送的是同一只 id)。本地 spool 是 WAL:出口挂死时投影照跑,
账不丢。
