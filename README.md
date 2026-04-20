# alasia — 动态 DNS 客户端 (C++23) v2

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD%20%7C%20macOS-yellow)](README.md)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue)](README.md)
[![Release](https://img.shields.io/badge/release-v2.0.0-green.svg)](https://github.com/your-org/alasia/releases)

> **Alasia** 名称源自 Aiolos 的变体，象征快速与灵动。  
> 本项目寓意**快速响应网络变化，灵动更新 DNS 记录**。

[alasia](./alasia) 是一个用 C++23 编写的轻量级动态 DNS (DDNS) 客户端，支持多域名、多服务商、IPv6，具备跨平台能力和丰富的日志输出。

---

## 📑 目录

- [核心特性](#-核心特性)
- [支持的 DNS 服务商](#-支持的 dns 服务商)
- [快速开始](#-快速开始)
- [安装指南](#-安装指南)
- [配置详解](#-配置详解)
- [命令行参数](#-命令行参数)
- [使用示例](#-使用示例)
- [高级功能](#-高级功能)
- [故障排查](#-故障排查)

---

## ✨ 核心特性

### 🚀 基础特性

- **多平台支持**：Linux、macOS、FreeBSD、OpenBSD
- **多 DNS 服务商**：Cloudflare、阿里云 DNS
- **并发更新**：同时更新多条 DNS 记录，提高效率
- **IPv6 优先**：原生支持 IPv6 地址动态获取与更新
- **高性能**：CURL 连接池，TCP Keepalive

### 🔒 安全特性

- **配置集中管理**：所有敏感信息在配置文件 `environment` 字段中统一管理
- **日志脱敏**：自动隐藏敏感信息，防止泄露

### 🛠️ 运维友好

- **多种部署方式**：systemd、Crontab、launchd
- **缓存机制**：IP 未变化时不触发 API 更新
- **灵活日志**：支持标准输出或文件
- **代理支持**：Cloudflare 支持 HTTP/SOCKS5 代理

---

## 🌐 支持的 DNS 服务商

### Cloudflare

| 特性 | 说明 |
|------|------|
| **认证方式** | API Token |
| **所需权限** | `Zone:DNS:Edit` |
| **代理支持** | ✅ 支持 HTTP/HTTPS/SOCKS5 |
| **Zone ID** | 可自动获取或手动配置 |
| **代理模式** | 支持 Cloudflare CDN 代理（`proxied` 字段） |
| **最小 TTL** | 120 秒 |

**API Token 获取**：[https://dash.cloudflare.com/profile/api-tokens](https://dash.cloudflare.com/profile/api-tokens)

### 阿里云 DNS

| 特性 | 说明 |
|------|------|
| **认证方式** | AccessKey ID + AccessKey Secret |
| **所需权限** | `AliyunDNSFullAccess` |
| **代理支持** | ❌ 不支持 |
| **签名方式** | HMAC-SHA1 |
| **TTL 范围** | 1-86400 秒 |

**AccessKey 获取**：[https://ram.console.aliyun.com/manage/ak](https://ram.console.aliyun.com/manage/ak)

---

## 🚀 快速开始

### 1. 安装依赖

#### Ubuntu / Debian
```bash
sudo apt install cmake build-essential libcurl4-openssl-dev libssl-dev
```

#### FreeBSD
```bash
pkg install cmake gcc curl openssl
```

#### macOS
```bash
brew install cmake curl openssl
```

### 2. 构建

```bash
# 基础构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 使用构建脚本
./build.sh v2.0.0
```

### 3. 配置

复制示例配置文件：

```bash
cp config.example.json config.json
```

编辑 `config.json`，在 `environment` 字段中填入你的凭证（详见 [配置详解](#-配置详解)）。

### 4. 运行

```bash
./build/alasia run -f config.json
```

---

## 📦 安装指南

### systemd 部署（推荐）

#### 1. 创建配置目录

```bash
sudo mkdir -p /etc/alasia
sudo chmod 755 /etc/alasia
```

#### 2. 创建配置文件

```bash
sudo cp config.example.json /etc/alasia/config.json
sudo chmod 600 /etc/alasia/config.json
```

编辑 `/etc/alasia/config.json`，在 `environment` 字段中定义敏感信息。

#### 3. 创建 systemd 服务文件

创建 `/etc/systemd/system/alasia.service`：

```ini
[Unit]
Description=Alasia DDNS Client
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/alasia run -f /etc/alasia/config.json
WorkingDirectory=/etc/alasia

[Install]
WantedBy=multi-user.target
```

#### 4. 创建 systemd 定时器

创建 `/etc/systemd/system/alasia.timer`：

```ini
[Unit]
Description=Run Alasia DDNS every 10 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=10min
Unit=alasia.service

[Install]
WantedBy=timers.target
```

#### 5. 启用并启动

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now alasia.timer
```

---

### Crontab 部署

#### 1. 编辑 crontab

```bash
crontab -e
```

#### 2. 添加定时任务

```bash
# 每 10 分钟执行一次
*/10 * * * * /usr/local/bin/alasia run -f /etc/alasia/config.json
```

---

### macOS launchd 部署

创建 `/Library/LaunchDaemons/com.alasia.plist`：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.alasia</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/alasia</string>
        <string>run</string>
        <string>-f</string>
        <string>/etc/alasia/config.json</string>
    </array>
    <key>StartInterval</key>
    <integer>600</integer>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
```

加载服务：

```bash
sudo launchctl load /Library/LaunchDaemons/com.alasia.plist
```

---

## 📝 配置详解

### 配置文件结构

```json
{
    "environment": {
        // 环境变量和敏感值集中存放
    },
    "general": {
        // 全局配置
    },
    "records": [
        // DNS 记录列表
    ]
}
```

### 完整配置示例

```json
{
    "environment": {
        "cloudflare_token": "your_cloudflare_api_token_here",
        "cloudflare_zone_id": "your_cloudflare_zone_id_here",
        "aliyun_key_id": "your_aliyun_access_key_id",
        "aliyun_key_secret": "your_aliyun_access_key_secret"
    },
    "general": {
        "get_ip": {
            "interface": "eth0",
            "urls": [
                "https://ipv6.icanhazip.com",
                "https://6.ipw.cn"
            ]
        },
        "work_dir": "/opt/alasia",
        "log_output": "shell",
        "proxy": ""
    },
    "records": [
        {
            "provider": "cloudflare",
            "zone": "example.com",
            "record": "dev",
            "ttl": 300,
            "proxied": false,
            "use_proxy": false,
            "cloudflare": {
                "api_token": "$cloudflare_token",
                "zone_id": "$cloudflare_zone_id"
            }
        },
        {
            "provider": "aliyun",
            "zone": "example.cn",
            "record": "www",
            "ttl": 600,
            "use_proxy": false,
            "aliyun": {
                "access_key_id": "$aliyun_key_id",
                "access_key_secret": "$aliyun_key_secret"
            }
        }
    ]
}
```

### 字段说明

#### 顶层字段

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| `environment` | `map[string]string` | 否 | 环境变量和敏感值集中存放，可在配置中引用 |
| `general` | `GeneralConfig` | 是 | 全局配置 |
| `records` | `[]RecordConfig` | 是 | DNS 记录列表（至少一条） |

#### general 字段

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| `get_ip.interface` | `string` | 条件 | 网卡名称（与 `urls` 二选一） |
| `get_ip.urls` | `[]string` | 条件 | IP 获取 API URLs（与 `interface` 二选一） |
| `work_dir` | `string` | 否 | 工作目录（存放缓存文件） |
| `log_output` | `string` | 否 | 日志输出路径，`shell` 表示标准输出 |
| `proxy` | `string` | 否 | 全局代理 URL（`socks5://` 或 `http://`） |

#### records 字段（每条记录）

| 字段 | 类型 | 必需 | 描述 |
|------|------|------|------|
| `provider` | `string` | 是 | 服务商：`cloudflare` 或 `aliyun` |
| `zone` | `string` | 是 | 主域名（如 `example.com`） |
| `record` | `string` | 是 | 子域名记录（如 `www`、`@`、`dev`） |
| `ttl` | `int` | 否 | TTL 值（秒） |
| `proxied` | `bool` | 否 | Cloudflare 代理模式（仅 Cloudflare） |
| `use_proxy` | `bool` | 否 | 是否使用全局代理（仅 Cloudflare 支持） |
| `cloudflare` | `object` | 条件 | Cloudflare 特定配置（`provider=cloudflare` 时必需） |
| `aliyun` | `object` | 条件 | 阿里云特定配置（`provider=aliyun` 时必需） |

---

### 变量引用语法

Alasia 仅支持引用配置文件顶层 `environment` 中定义的变量。

**语法**：使用 `$变量名` 方式引用

```json
{
    "environment": {
        "cloudflare_token": "your_api_token",
        "aliyun_key": "your_access_key"
    },
    "records": [
        {
            "cloudflare": {
                "api_token": "$cloudflare_token",
                "zone_id": "$cloudflare_zone_id"
            }
        }
    ]
}
```

**注意**：
- 引用格式：`$变量名`（例如：`$cloudflare_token`）
- 如果引用的变量在 `environment` 中不存在，程序将报错退出
- 不支持系统环境变量引用（如 `${ENV_VAR}`）
- 不支持变量默认值语法

---

## 🖥️ 命令行参数

### 命令结构

```bash
alasia <command> [options]
```

### 可用命令

| 命令 | 描述 |
|------|------|
| `run` | 运行 DDNS 更新 |
| `version` | 显示版本信息 |

### `run` 命令参数

| 参数 | 简写 | 类型 | 默认值 | 描述 |
|------|------|------|--------|------|
| `--config` | `-f` | `string` | 无 | 配置文件路径（JSON 格式） |
| `--ignore-cache` | `-i` | `bool` | `false` | 忽略缓存 IP，强制更新 |
| `--timeout` | `-t` | `int` | `300` | 超时时间（秒） |

### `version` 命令

显示版本、提交哈希和构建日期：

```bash
alasia version
```

---

## 💡 使用示例

### 基本用法

```bash
# 指定配置文件
alasia run -f /etc/alasia/config.json

# 强制更新（忽略缓存）
alasia run -f config.json -i

# 使用自定义超时时间
alasia run -f config.json -t 600
```

---

## 🔧 高级功能

### IP 获取策略

#### 网卡方式（优先）

直接从指定网卡获取 IPv6 地址：

```json
{
    "general": {
        "get_ip": {
            "interface": "eth0"
        }
    }
}
```

#### API 方式（备用）

通过多个公共 API 并发查询：

```json
{
    "general": {
        "get_ip": {
            "urls": [
                "https://ipv6.icanhazip.com",
                "https://6.ipw.cn"
            ]
        }
    }
}
```

### 缓存机制

- **缓存文件**：`{work_dir}/cache.lastip`
- **作用**：避免 IP 未变化时频繁调用 API
- **跳过缓存**：使用 `-i` 或 `--ignore-cache` 参数

### 日志系统

#### 日志输出配置

```json
{
    "general": {
        "log_output": "shell"
    }
}
```

输出到文件：

```json
{
    "general": {
        "log_output": "/var/log/alasia.log"
    }
}
```

### 代理支持

#### 全局代理配置

```json
{
    "general": {
        "proxy": "socks5://127.0.0.1:1080"
    }
}
```

> ⚠️ **注意**：仅 Cloudflare 支持代理，阿里云不支持。

### 并发更新

所有 DNS 记录并发更新，提高多域名场景下的更新效率。

---

## 🐛 故障排查

### 常见问题

#### 1. 配置解析失败

**错误信息**：
```
Config: record[0]: cloudflare.api_token is not set or empty
```

**解决方案**：
- 检查 `environment` 中是否定义了对应变量
- 检查引用名称是否正确（`$cloudflare_token`）

#### 2. IP 获取失败

**错误信息**：
```
Failed to get current IP
```

**解决方案**：
- 检查网卡名称是否正确（`ip addr` 查看）
- 确保系统已启用 IPv6
- 尝试使用 API 方式获取（配置 `urls` 字段）

#### 3. Cloudflare API 权限不足

**错误信息**：
```
Cloudflare upsert failed: Invalid API Token
```

**解决方案**：
- 检查 API Token 是否正确
- 确保 Token 具有 `Zone:DNS:Edit` 权限
- 检查 Zone ID 是否正确

### 日志级别

调试模式下查看详细请求信息。

---

## 平台支持

| 平台 | IPv6 获取方式 | 状态 |
|------|--------------|------|
| Linux | netlink (`RTM_GETADDR`) | ✅ 完整支持 |
| FreeBSD | ioctl | ✅ 支持 |
| OpenBSD | ioctl | ✅ 支持 |
| macOS | ioctl | ⚠️ **实验性支持** |

> **macOS 说明**：macOS 支持目前为实验性质。理论上代码可正常工作，但可能有未发现问题。

所有平台均支持 HTTP API 降级方式获取 IPv6 地址。

---

## 目录结构

```
alasia/
├── CMakeLists.txt
├── build.sh
├── config.example.json
├── LICENSE
├── README.md
└── src/
    ├── main.cpp
    ├── log.hpp / log.cpp
    ├── config.hpp / config.cpp
    ├── cache.cpp
    ├── curl_pool.cpp
    ├── ip_getter.hpp / ip_getter.cpp
    ├── ip_getter_bsd.cpp
    └── provider/
        ├── provider.hpp
        ├── cloudflare.hpp / cloudflare.cpp
        └── aliyun.hpp / aliyun.cpp
```

---

## 许可证

采用 **MIT License** - 详见 [LICENSE](LICENSE) 文件。

---

**Made with ❤️ by the Alasia Team**
