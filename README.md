# Alasia — 轻量级动态 DNS 客户端

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20macOS-yellow)](README.md)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue)](README.md)

> **Alasia** 名称源自 Aiolos 的变体，象征快速与灵动。寓意**快速响应网络变化，灵动更新 DNS 记录**。

一个用现代 C++23 编写的高性能动态 DNS (DDNS) 客户端，支持多域名并发更新、IPv6 优先、跨平台部署。

## 特性

- **多平台**：Linux、FreeBSD、macOS
- **多服务商**：Cloudflare、阿里云 DNS（架构易扩展）
- **IPv6 优先**：支持从网卡或 HTTP API 获取 IPv6 地址，自动过滤链路本地/ULA/环回地址
- **并发更新**：多条 DNS 记录同时更新
- **缓存机制**：IP 未变化时不触发 API 调用
- **敏感信息管理**：`environment` 集中定义，通过 `$变量名` 引用，日志自动脱敏
- **代理支持**：Cloudflare 支持 HTTP/SOCKS5 代理（仅 Cloudflare）
- **连接池**：CURL 连接池 + TCP Keepalive

## 快速开始

### 1. 构建

```bash
./build.sh          # 开发版本
./build.sh 2.1.1    # 指定版本
```

验证：

```bash
./build/alasia version
```

### 2. 配置

```bash
cp config.example.json config.json
```

完整配置说明见下方 [配置](#配置) 一节。

### 3. 运行

```bash
./build/alasia run -c config.json -d /etc/alasia
```

## 配置

配置文件为 JSON 格式。以下示例使用 JSONC 语法（带注释）讲解字段，实际使用时请复制 `config.example.json` 并填入自己的值。

```jsonc
{
  // ── environment ──────────────────────────────────────────
  // 敏感信息集中存放，通过 $变量名 在 records 中引用。
  // 仅支持 $name 语法，不支持 ${name} 或系统环境变量。
  "environment": {
    "cf_token": "your_cloudflare_api_token",
    "cf_zone": "your_cloudflare_zone_id",
    "ak_id": "your_aliyun_access_key_id",
    "ak_secret": "your_aliyun_access_key_secret"
  },

  // ── general ──────────────────────────────────────────────
  "general": {
    "get_ip": {
      "interface": "eth0",                               // 网卡名（与 urls 二选一，interface 优先）
      "urls": [                                          // HTTP API 回退方案
        "https://ipv6.icanhazip.com",
        "https://6.ipw.cn",
        "https://v6.ipv6-test.com/api/myip.php"
      ]
    },
    "proxy": ""                                          // 全局代理 socks5:// 或 http://（仅 Cloudflare 生效）
  },

  // ── records ─────────────────────────────────────────────
  "records": [
    {
      // 基础字段（所有服务商通用）
      "provider": "cloudflare",                          // cloudflare | aliyun
      "zone": "example.com",                             // 主域名
      "record": "www",                                   // 子域名，@ 表示根域名
      "ttl": 300,                                        // 可选，Cloudflare 默认 180，阿里云默认 600
      "proxied": false,                                  // 可选，Cloudflare CDN 代理
      "use_proxy": false,                                // 可选，是否使用 general.proxy

      // 服务商专属字段（按 provider 选择其一）
      "cloudflare": {
        "api_token": "$cf_token",                        // 必需，$ 引用 environment
        "zone_id": "$cf_zone"                            // 可选，留空自动获取
      },
      "aliyun": {
        "access_key_id": "$ak_id",                       // 必需
        "access_key_secret": "$ak_secret"                // 必需
      }
    }
  ]
}
```

### 服务商对比

| | Cloudflare | 阿里云 |
|--|------------|--------|
| **认证** | API Token | AccessKey ID + Secret |
| **权限** | `Zone:DNS:Edit` | `AliyunDNSFullAccess` |
| **代理** | ✅ HTTP/SOCKS5 | ❌ 不支持 |
| **Zone ID** | 留空自动获取 | — |

## 命令行

```
alasia <command> [options]
```

| 命令 | 说明 |
|------|------|
| `run` | 执行 DDNS 更新 |
| `version` | 显示版本信息 |

`run` 命令参数：

| 参数 | 简写 | 默认值 | 说明 |
|------|------|--------|------|
| `--config` | `-c` | 无 | 配置文件路径 |
| `--dir` | `-d` | 配置目录 | 工作目录（存放缓存文件 `cache.lastip`） |
| `--ignore-cache` | `-i` | false | 忽略缓存，强制更新 |
| `--timeout` | `-t` | 300 | 超时时间（秒） |

## 部署

### systemd（推荐）

`/etc/systemd/system/alasia.service`：

```ini
[Unit]
Description=Alasia DDNS Client
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/alasia run -c /etc/alasia/config.json
WorkingDirectory=/etc/alasia

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/alasia.timer`：

```ini
[Unit]
Description=Run Alasia DDNS every 10 minutes
Requires=alasia.service

[Timer]
OnBootSec=5min
OnUnitActiveSec=10min
Unit=alasia.service

[Install]
WantedBy=timers.target
```

启用：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now alasia.timer
```

### Crontab

```bash
*/10 * * * * /usr/local/bin/alasia run -c /etc/alasia/config.json -d /etc/alasia >> /var/log/alasia.log 2>&1
```

### macOS launchd

`/Library/LaunchDaemons/com.alasia.plist`：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.alasia</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/alasia</string>
        <string>run</string>
        <string>-c</string>
        <string>/etc/alasia/config.json</string>
    </array>
    <key>StartInterval</key>
    <integer>600</integer>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
```

加载：

```bash
sudo launchctl load /Library/LaunchDaemons/com.alasia.plist
```

## 平台支持

| 平台 | IPv6 获取（网卡） | IPv6 获取（HTTP API） | 状态 |
|------|-------------------|----------------------|------|
| Linux | ✅ netlink | ✅ | 完整支持 |
| FreeBSD | ✅ ioctl | ✅ | 完整支持 |
| OpenBSD | ❌ | ✅ | 仅 HTTP API |
| macOS | ❌ | ✅ | 仅 HTTP API |

## 故障排查

| 错误 | 解决方案 |
|------|----------|
| `Config: record[0]: cloudflare.api_token is not set` | 检查 `environment` 中变量是否定义，引用名是否正确 |
| `Failed to get current IP` | 检查网卡名（`ip addr`），确保 IPv6 已启用，或改用 `urls` API 方式 |
| `Invalid API Token` | 检查 Token 和 `Zone:DNS:Edit` 权限 |
| `InvalidSignature` | 检查 AccessKey，确保系统时间准确（NTP 同步） |

日志默认输出到 stdout，systemd 用户可通过 `journalctl -u alasia.service -f` 查看。

## 贡献与许可

欢迎提交 Issue 和 Pull Request。

项目采用 **MIT License** — 详见 [LICENSE](LICENSE) 文件。

致谢：[nlohmann/json](https://github.com/nlohmann/json)、[argparse](https://github.com/p-ranav/argparse)、[libcurl](https://curl.se/)、[OpenSSL](https://www.openssl.org/)
