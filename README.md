# Alasia — 轻量级动态 DNS 客户端

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD%20%7C%20macOS-yellow)](README.md)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue)](README.md)

> **Alasia** 名称源自 Aiolos 的变体，象征快速与灵动。寓意**快速响应网络变化，灵动更新 DNS 记录**。

一个用现代 C++23 编写的高性能动态 DNS (DDNS) 客户端，支持多域名并发更新、IPv6 优先、跨平台部署。

---

## ✨ 特性亮点

| 类别 | 特性 |
|------|------|
| 🚀 **性能** | 并发更新多条 DNS 记录、CURL 连接池、TCP Keepalive |
| 🔒 **安全** | 敏感信息集中管理、日志自动脱敏、缓存文件权限保护 (0600) |
| 🌐 **网络** | IPv6 优先、支持网卡直连或 HTTP API 获取 IP |
| 🛠️ **部署** | systemd / Crontab / launchd 多种部署方式 |
| 📦 **服务商** | Cloudflare、阿里云 DNS |

---

## 📋 目录

- [快速开始](#-快速开始)
- [配置指南](#-配置指南)
- [命令行参数](#-命令行参数)
- [部署方式](#-部署方式)
- [故障排查](#-故障排查)
- [技术架构](#-技术架构)

---

## 🚀 快速开始

### 1. 安装依赖

**Ubuntu / Debian**
```bash
sudo apt install cmake build-essential libcurl4-openssl-dev libssl-dev
```

**FreeBSD**
```bash
pkg install cmake gcc curl openssl
```

**macOS**
```bash
brew install cmake curl openssl
```

### 2. 编译

#### 使用构建脚本（推荐）

```bash
# 默认使用 GCC 编译
./build.sh

# 指定版本号
./build.sh 2.1.0

# 使用 Clang++ 编译
CXX_COMPILER=clang++ C_COMPILER=clang ./build.sh 2.1.0
```

> **提示**：如果指定的编译器不存在，脚本会提示你是否切换回默认编译器。

#### 手动编译

```bash
# 使用 GCC 编译（默认）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 使用 Clang++ 编译
cmake -B build \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release

# 使用其他编译器（如 ccache + g++）
cmake -B build \
  -DCMAKE_CXX_COMPILER="ccache g++" \
  -DCMAKE_C_COMPILER="ccache gcc" \
  -DCMAKE_BUILD_TYPE=Release
```

#### 编译选项

| 编译器 | CMAKE_C_COMPILER | CMAKE_CXX_COMPILER | 说明 |
|--------|------------------|---------------------|------|
| GCC（默认） | `gcc` | `g++` | 推荐用于 Linux |
| Clang | `clang` | `clang++` | 更好的错误提示 |
| Clang + ccache | `ccache gcc` | `ccache g++` | 加速重复编译 |

编译产物：`build/alasia`

### 3. 配置

复制示例配置：
```bash
cp config.example.json config.json
```

编辑 `config.json`，在 `environment` 字段中填入你的 API 凭证（详见 [配置指南](#-配置指南)）。

### 4. 运行

```bash
./build/alasia run -c config.json
```

---

## 📝 配置指南

### 配置结构

配置文件为 JSON 格式，包含三个主要部分：

```json
{
    "environment": { /* 敏感信息集中存放 */ },
    "general": { /* 全局配置 */ },
    "records": [ /* DNS 记录列表 */ ]
}
```

### 完整示例

```json
{
    "environment": {
        "cloudflare_token": "你的 Cloudflare API Token",
        "cloudflare_zone_id": "你的 Zone ID",
        "aliyun_key_id": "你的 AccessKey ID",
        "aliyun_key_secret": "你的 AccessKey Secret"
    },
    "general": {
        "get_ip": {
            "interface": "eth0"
        },
        "log_output": "shell",
        "proxy": ""
    },
    "records": [
        {
            "provider": "cloudflare",
            "zone": "example.com",
            "record": "www",
            "ttl": 300,
            "cloudflare": {
                "api_token": "$cloudflare_token",
                "zone_id": "$cloudflare_zone_id"
            }
        },
        {
            "provider": "aliyun",
            "zone": "example.cn",
            "record": "home",
            "ttl": 600,
            "aliyun": {
                "access_key_id": "$aliyun_key_id",
                "access_key_secret": "$aliyun_key_secret"
            }
        }
    ]
}
```

### 字段说明

#### `environment` - 敏感信息

所有 API 凭证、Token 等敏感信息在此集中定义，通过 `$变量名` 方式引用。

```json
{
    "environment": {
        "cloudflare_token": "vF5k...",
        "aliyun_key_id": "LTAI5t...",
        "aliyun_key_secret": "xK9m..."
    }
}
```

#### `general` - 全局配置

| 字段 | 类型 | 说明 |
|------|------|------|
| `get_ip.interface` | string | 网卡名称（如 `eth0`、`en0`），与 `urls` 二选一 |
| `get_ip.urls` | string[] | IP 查询 API 列表（如 `https://ipv6.icanhazip.com`） |
| `log_output` | string | 日志输出：`shell` 或文件路径（相对于 `--dir`） |
| `proxy` | string | 全局代理 URL（仅 Cloudflare 支持） |

#### `records` - DNS 记录列表

每条记录包含：

| 字段 | 类型 | 说明 |
|------|------|------|
| `provider` | string | 服务商：`cloudflare` 或 `aliyun` |
| `zone` | string | 主域名（如 `example.com`） |
| `record` | string | 子域名（如 `www`、`@`、`home`） |
| `ttl` | number | TTL 时间（秒），Cloudflare 最小 120 |
| `proxied` | boolean | Cloudflare CDN 代理（仅 Cloudflare） |
| `cloudflare` | object | Cloudflare 配置（见下） |
| `aliyun` | object | 阿里云配置（见下） |

**Cloudflare 记录配置**
```json
{
    "provider": "cloudflare",
    "zone": "example.com",
    "record": "www",
    "ttl": 300,
    "proxied": false,
    "cloudflare": {
        "api_token": "$cloudflare_token",
        "zone_id": "$cloudflare_zone_id"
    }
}
```

**阿里云记录配置**
```json
{
    "provider": "aliyun",
    "zone": "example.cn",
    "record": "home",
    "ttl": 600,
    "aliyun": {
        "access_key_id": "$aliyun_key_id",
        "access_key_secret": "$aliyun_key_secret"
    }
}
```

### 变量引用

使用 `$变量名` 引用 `environment` 中定义的值：

```json
{
    "environment": {
        "my_token": "secret_value"
    },
    "records": [{
        "cloudflare": {
            "api_token": "$my_token"
        }
    }]
}
```

---

## 🖥️ 命令行参数

### `run` 命令

运行 DDNS 更新：

```bash
alasia run [选项]
```

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--config` | `-c` | string | 无 | 配置文件路径 |
| `--dir` | `-d` | string | 配置目录 | 工作目录（缓存、日志） |
| `--ignore-cache` | `-i` | boolean | false | 忽略缓存，强制更新 |
| `--timeout` | `-t` | number | 300 | 超时时间（秒） |

**示例**
```bash
# 使用指定配置
alasia run -c /etc/alasia/config.json

# 强制更新，忽略缓存
alasia run -c config.json -i

# 自定义超时时间
alasia run -c config.json -t 600
```

### `version` 命令

显示版本信息：
```bash
alasia version
```

---

## 🛠️ 部署方式

### systemd 部署（推荐）

#### 1. 创建目录和配置
```bash
sudo mkdir -p /etc/alasia
sudo cp config.example.json /etc/alasia/config.json
sudo chmod 600 /etc/alasia/config.json
```

#### 2. 创建服务文件 `/etc/systemd/system/alasia.service`
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

#### 3. 创建定时器 `/etc/systemd/system/alasia.timer`
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

#### 4. 启用服务
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now alasia.timer
```

---

### Crontab 部署

```bash
crontab -e
```

添加任务（每 10 分钟执行）：
```bash
*/10 * * * * /usr/local/bin/alasia run -c /etc/alasia/config.json
```

---

### macOS launchd 部署

创建 `/Library/LaunchDaemons/com.alasia.plist`：

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

加载服务：
```bash
sudo launchctl load /Library/LaunchDaemons/com.alasia.plist
```

---

## 🐛 故障排查

### 常见问题

#### 配置解析失败
```
Config: record[0]: cloudflare.api_token is not set or empty
```
**解决**：检查 `environment` 中是否定义变量，引用名称是否正确。

#### IP 获取失败
```
Failed to get current IP
```
**解决**：
- 检查网卡名称（`ip addr` 或 `ifconfig`）
- 确保 IPv6 已启用
- 改用 HTTP API 方式（配置 `urls` 字段）

#### Cloudflare API 权限不足
```
Invalid API Token
```
**解决**：
- 确认 Token 具有 `Zone:DNS:Edit` 权限
- 检查 Zone ID 是否正确

#### 阿里云签名失败
```
InvalidSignature
```
**解决**：
- 检查 AccessKey ID 和 Secret 是否正确
- 确保系统时间准确（签名依赖时间戳）

---

## 🏗️ 技术架构

### 目录结构

```
alasia/
├── CMakeLists.txt          # CMake 构建配置
├── build.sh                # 构建脚本
├── config.example.json     # 配置示例
├── LICENSE                 # MIT 许可证
├── README.md               # 本文档
└── src/
    ├── main.cpp            # 程序入口
    ├── commands/           # 命令行处理
    ├── config/             # 配置解析
    ├── core/               # 核心类型定义
    ├── http/               # HTTP 客户端
    ├── provider/           # DNS 服务商实现
    └── services/           # 服务层（IP/DNS/缓存）
```

### 核心模块

| 模块 | 职责 |
|------|------|
| `commands` | 命令行参数解析与命令分发 |
| `config` | JSON 配置解析、环境变量展开、验证 |
| `http` | CURL 连接池、HTTP 请求封装 |
| `provider` | Cloudflare / 阿里云 API 实现 |
| `services/ip` | IPv6 地址获取（网卡/netlink 或 HTTP API） |
| `services/dns` | DNS 记录查询与更新 |
| `services/cache` | IP 缓存（避免频繁 API 调用） |

### 技术栈

- **语言标准**：C++23
- **构建系统**：CMake
- **JSON 库**：nlohmann/json
- **HTTP 库**：libcurl
- **加密库**：OpenSSL（阿里云 HMAC-SHA1 签名）

---

## 📊 平台支持

| 平台 | IPv6 获取 | 状态 |
|------|----------|------|
| Linux | netlink (`RTM_GETADDR`) | ✅ 完整支持 |
| FreeBSD | ioctl | ✅ 支持 |
| OpenBSD | ioctl | ✅ 支持 |
| macOS | ioctl | ⚠️ 实验性支持 |

所有平台均支持 HTTP API 降级方式获取 IPv6。

---

## 🔒 安全说明

1. **文件权限**：缓存文件自动设置为 `0600`（仅所有者可读写）
2. **日志脱敏**：敏感信息自动隐藏，防止泄露
3. **配置保护**：建议配置文件权限设置为 `600`
4. **密钥管理**：所有凭证在 `environment` 中集中管理，便于权限控制

---

## 📄 许可证

采用 **MIT License** - 详见 [LICENSE](LICENSE) 文件。

---

**Made with ❤️ by the Alasia Team**
