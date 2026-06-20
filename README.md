# Gecko-ddns — 轻量级动态 DNS 客户端

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD%20%7C%20macOS-yellow)](README.md)
[![Version](https://img.shields.io/badge/version-2.0-green)](README.md)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue)](README.md)

**Gecko-ddns** 是一个用现代 C++23 编写的高性能动态 DNS (DDNS) 客户端，支持多平台、多服务商、IPv6 优先。名称取自壁虎（Gecko）— 轻巧灵活，适应各种环境。

## 特性

- **多平台**：Linux、FreeBSD、OpenBSD、macOS
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
./build.sh          # 编译 2.0 版本
./build.sh 2.0      # 或显式指定版本
```

验证：

```bash
./build/gecko-ddns version
```

### 2. 配置

```bash
cp config.example.json config.json
```

完整配置说明见下方 [配置](#配置) 一节。

### 3. 运行

```bash
./build/gecko-ddns run -c config.json -d /etc/gecko-ddns
```

## 配置

配置文件为 JSON 格式。以下示例使用 JSONC 语法（带注释）讲解字段，实际使用时请复制 `config.example.json` 并填入自己的值。

```jsonc
{
  // ── env ────────────────────────────────────────────────
  // 敏感信息集中存放，通过 $变量名 在 records 中引用。
  // 仅支持 $name 语法，不支持 ${name} 或系统环境变量。
  "env": {
    "cf_token": "your_cloudflare_api_token",
    "cf_zone": "your_cloudflare_zone_id",
    "ak_id": "your_aliyun_access_key_id",
    "ak_secret": "your_aliyun_access_key_secret"
  },

  // ── ip_source ──────────────────────────────────────────
  // interface 优先；fallback_urls 永不使用代理。
  "ip_source": {
    "interface": "eth0",
    "fallback_urls": [
      "https://ipv6.icanhazip.com",
      "https://6.ipw.cn"
    ]
  },

  // ── proxy ──────────────────────────────────────────────
  // 全局代理（可选），仅用于 DNS API 请求（Cloudflare）。
  // 不影响 ip_source 的 IP 探测。
  "proxy": "",

  // ── records ────────────────────────────────────────────
  // 各 provider 字段平铺，运行期按 provider 校验。
  "records": [
    {
      "provider": "cloudflare",
      "zone": "example.com",
      "name": "www",
      "type": "AAAA",
      "ttl": 300,
      "proxied": false,
      "use_proxy": false,

      "api_token": "$cf_token",
      "zone_id": "$cf_zone"
    },
    {
      "provider": "aliyun",
      "zone": "example.cn",
      "name": "www",
      "type": "AAAA",
      "ttl": 600,
      "use_proxy": false,

      "access_key_id": "$ak_id",
      "access_key_secret": "$ak_secret"
    }
  ]
}
```

### 服务商对比

| | Cloudflare | 阿里云 |
|--|------------|--------|
| **认证** | API Token | AccessKey ID + Secret |
| **权限** | `Zone:DNS:Edit` | `AliyunDNSFullAccess` |
| **代理** | ✅ HTTP/SOCKS5 | ✅ HTTP/SOCKS5 |
| **Zone ID** | 留空自动获取 | — |

## 命令行

```
gecko-ddns <command> [options]
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

`/etc/systemd/system/gecko-ddns.service`：

```ini
[Unit]
Description=Gecko-ddns DDNS Client
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/gecko-ddns run -c /etc/gecko-ddns/config.json
WorkingDirectory=/etc/gecko-ddns

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/gecko-ddns.timer`：

```ini
[Unit]
Description=Run Gecko-ddns DDNS every 10 minutes
Requires=gecko-ddns.service

[Timer]
OnBootSec=5min
OnUnitActiveSec=10min
Unit=gecko-ddns.service

[Install]
WantedBy=timers.target
```

启用：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now gecko-ddns.timer
```

### Crontab

```bash
*/10 * * * * /usr/local/bin/gecko-ddns run -c /etc/gecko-ddns/config.json -d /etc/gecko-ddns >> /var/log/gecko-ddns.log 2>&1
```

### macOS launchd

`/Library/LaunchDaemons/com.gecko-ddns.plist`：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.gecko-ddns</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/gecko-ddns</string>
        <string>run</string>
        <string>-c</string>
        <string>/etc/gecko-ddns/config.json</string>
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
sudo launchctl load /Library/LaunchDaemons/com.gecko-ddns.plist
```

## 平台支持

| 平台 | IPv6 获取（网卡） | IPv6 获取（HTTP API） | 状态 |
|------|-------------------|----------------------|------|
| Linux | ✅ Netlink 原生 | ✅ | 完整支持 |
| FreeBSD | ✅ Netlink SNL API | ✅ | 完整支持 |
| OpenBSD | ✅ ioctl | ✅ | 完整支持 |
| macOS | ❌ | ✅ | 仅 HTTP API |

## v2.0 更新说明

**项目更名**：`alasia` → `gecko-ddns`

**新增 OpenBSD 支持**
- 通过 `SIOCGIFALIFETIME_IN6` ioctl 获取 IPv6 地址和生命周期
- 支持 SLAAC 临时地址和 autoconf 地址的 pltime/vltime 识别

**FreeBSD 使用 Netlink**
- 从传统 ioctl 迁移至 FreeBSD 14+ 原生 Netlink SNL API（`snl_parse_nlmsg` / `snl_rtm_addr_parser`）
- 与 `ifconfig` 使用相同的内核路径获取地址信息，结果更准确

**平台代码独立**
- Linux、FreeBSD、OpenBSD 各平台 IPv6 获取逻辑分离到独立文件
- 新增平台只需添加源文件 + CMake 分支即可接入

## 故障排查

| 错误 | 解决方案 |
|------|----------|
| `Config: record[0]: cloudflare.api_token is not set` | 检查 `environment` 中变量是否定义，引用名是否正确 |
| `Failed to get current IP` | 检查网卡名（`ip addr`），确保 IPv6 已启用，或改用 `urls` API 方式 |
| `Invalid API Token` | 检查 Token 和 `Zone:DNS:Edit` 权限 |
| `InvalidSignature` | 检查 AccessKey，确保系统时间准确（NTP 同步） |

日志默认输出到 stdout，systemd 用户可通过 `journalctl -u gecko-ddns.service -f` 查看。

## 贡献与许可

欢迎提交 Issue 和 Pull Request。

项目采用 **MIT License** — 详见 [LICENSE](LICENSE) 文件。

致谢：[nlohmann/json](https://github.com/nlohmann/json)、[argparse](https://github.com/p-ranav/argparse)、[libcurl](https://curl.se/)、[OpenSSL](https://www.openssl.org/)
