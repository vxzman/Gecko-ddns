# alasia — 动态 DNS 客户端 (C++23)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD%20%7C%20macOS-yellow)](README.md)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue)](README.md)

[alasia](./alasia) 是一个用 C++23 编写的轻量级动态 DNS (DDNS) 客户端，支持多域名、多服务商、IPv6，具备跨平台能力和丰富的日志输出。

## 特性

- **多域名支持**：单次运行可并发更新多个 DNS 记录
- **Cloudflare 集成**：AAAA 记录自动创建/更新，Zone ID 自动获取
- **阿里云 DNS**：HMAC-SHA1 签名，AAAA 记录自动创建/更新
- **IPv6 支持**：
  - Linux：netlink 接口
  - FreeBSD/OpenBSD/macOS：ioctl 接口
  - 所有平台：HTTP API 降级
- **代理支持**：HTTP/HTTPS/SOCKS5（仅 Cloudflare）
- **IP 缓存**：避免重复 API 调用
- **彩色日志**：终端下分级彩色显示，支持文件输出
- **CURL 连接池**：线程局部缓存，TCP Keepalive，提升多域名场景性能

## 系统依赖

### Linux (Ubuntu / Debian)
```bash
sudo apt install cmake build-essential libcurl4-openssl-dev libssl-dev
```

### FreeBSD
```bash
pkg install cmake gcc curl openssl
```

### OpenBSD
```bash
pkg_add cmake curl openssl
```

### macOS
```bash
brew install cmake curl openssl
```

> `nlohmann/json` 和 `argparse` 由 CMake FetchContent 自动拉取，无需手动安装。

## 构建

### 使用 g++ 编译（默认）

```bash
# 直接构建（dev 版本）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 或指定使用 g++
CXX=g++ cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 使用 clang++ 编译

```bash
CXX=clang++ cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### macOS 构建

macOS 默认使用 clang++，需要指定 OpenSSL 路径：

```bash
# 安装依赖
brew install cmake curl openssl

# 构建时指定 OpenSSL 路径
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
cmake --build build -j$(sysctl -n hw.ncpu)
```

### 带版本信息构建

```bash
# 使用构建脚本
./build.sh v1.0.0

# 或手动设置编译参数
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DAPP_VERSION=v1.0.0 \
  -DAPP_COMMIT=$(git rev-parse HEAD) \
  -DAPP_BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cmake --build build -j$(nproc)
```

## 运行

```bash
# 更新 DNS 记录（使用缓存，IP 未变则跳过）
./build/alasia run -f config.json

# 强制更新（忽略缓存）
./build/alasia run -f config.json -i

# 查看版本
./build/alasia version
```

## 配置文件

### 安全性要求

**重要**：出于安全考虑，alasia **禁止在配置文件中明文存储密钥信息**。
所有敏感信息（API Token、AccessKey 等）必须使用环境变量引用。

❌ 错误示例（明文密钥，会被拒绝执行）：
```json
{
    "cloudflare": {
        "api_token": "your_actual_token_here"
    }
}
```

✅ 正确示例（使用环境变量）：
```json
{
    "cloudflare": {
        "api_token": "${CLOUDFLARE_API_TOKEN}",
        "zone_id": "${CLOUDFLARE_ZONE_ID:-}"
    }
}
```

运行前设置环境变量：
```bash
export CLOUDFLARE_API_TOKEN="your_token_here"
export ALIYUN_ACCESS_KEY_ID="LTAI1234567890"
export ALIYUN_ACCESS_KEY_SECRET="your_secret_here"
./build/alasia run -f config.json
```

### 配置示例

```json
{
    "general": {
        "get_ip": {
            "interface": "eth0",
            "urls": [
                "https://ipv6.icanhazip.com",
                "https://6.ipw.cn"
            ]
        },
        "work_dir": "/opt/dynip",
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
                "api_token": "${CLOUDFLARE_API_TOKEN}",
                "zone_id": "${CLOUDFLARE_ZONE_ID:-}"
            }
        },
        {
            "provider": "aliyun",
            "zone": "example.cn",
            "record": "www",
            "ttl": 600,
            "use_proxy": false,
            "aliyun": {
                "access_key_id": "${ALIYUN_ACCESS_KEY_ID}",
                "access_key_secret": "${ALIYUN_ACCESS_KEY_SECRET}"
            }
        }
    ]
}
```

## 配置字段说明

### general

| 字段 | 说明 |
|------|------|
| `get_ip.interface` | 本地网卡名（优先） |
| `get_ip.urls` | 外部 IPv6 检测 API 列表（降级） |
| `work_dir` | 缓存文件目录（空则与配置文件同目录） |
| `log_output` | `shell` 输出到终端，或日志文件路径 |
| `proxy` | 全局代理（socks5://... 或 http://...） |

### records

| 字段 | 说明 |
|------|------|
| `provider` | `cloudflare` 或 `aliyun` |
| `zone` | 主域名 |
| `record` | 子域名（`@` 表示根域） |
| `ttl` | TTL（秒） |
| `use_proxy` | 是否使用全局代理（仅 Cloudflare） |

## 自动运行

### systemd 定时（推荐）

#### 1. 创建配置文件

```bash
sudo mkdir -p /etc/alasia
sudo nano /etc/alasia/config.json
```

#### 2. 创建 systemd 服务和定时器

```ini
# /etc/systemd/system/alasia.service
[Unit]
Description=Dynamic DNS client
After=network.target

[Service]
Type=oneshot
Environment="CLOUDFLARE_API_TOKEN=your_token_here"
ExecStart=/usr/local/bin/alasia run -f /etc/alasia/config.json
WorkingDirectory=/etc/alasia
StandardOutput=append:/var/log/alasia.log
StandardError=append:/var/log/alasia.log
Restart=no

[Install]
WantedBy=multi-user.target
```

```ini
# /etc/systemd/system/alasia.timer
[Unit]
Description=Run alasia every 5 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min
Persistent=true

[Install]
WantedBy=timers.target
```

> **提示**：如需配置多个环境变量，在 `Environment=` 行添加，例如：
> ```ini
> Environment="CLOUDFLARE_API_TOKEN=xxx" "ALIYUN_ACCESS_KEY_ID=xxx" "ALIYUN_ACCESS_KEY_SECRET=xxx"
> ```

#### 3. 启用定时器

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now alasia.timer
```

---

### cron 定时

#### 1. 创建执行脚本

```bash
#!/bin/bash
# /etc/alasia/run-alasia.sh

# 配置环境变量
export CLOUDFLARE_API_TOKEN="your_token_here"
export ALIYUN_ACCESS_KEY_ID="your_access_key_id"
export ALIYUN_ACCESS_KEY_SECRET="your_access_key_secret"

# 执行 alasia
/usr/local/bin/alasia run -f /etc/alasia/config.json
```

设置权限：
```bash
sudo chmod +x /etc/alasia/run-alasia.sh
```

#### 2. 配置 crontab

```bash
sudo crontab -e
```

添加以下内容（每 5 分钟执行一次）：

```cron
*/5 * * * * /etc/alasia/run-alasia.sh >> /var/log/alasia.log 2>&1
```

> **提示**：根据实际需求调整执行频率。

---

### macOS launchd 定时

macOS 使用 `launchd` 管理后台任务，支持开机启动和定时执行。

#### 1. 创建配置文件

```bash
sudo mkdir -p /etc/alasia
sudo nano /etc/alasia/config.json
```

#### 2. 创建 launchd plist

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
    <key>EnvironmentVariables</key>
    <dict>
        <key>CLOUDFLARE_API_TOKEN</key>
        <string>your_token_here</string>
        <key>ALIYUN_ACCESS_KEY_ID</key>
        <string>your_access_key_id</string>
        <key>ALIYUN_ACCESS_KEY_SECRET</key>
        <string>your_access_key_secret</string>
    </dict>
    <key>StartInterval</key>
    <integer>300</integer>  <!-- 每 300 秒（5 分钟）执行一次 -->
    <key>RunAtLoad</key>
    <true/>  <!-- 加载时立即执行一次 -->
    <key>StandardOutPath</key>
    <string>/var/log/alasia.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/alasia.log</string>
</dict>
</plist>
```

#### 3. 加载并启动

```bash
# 设置正确权限
sudo chown root:wheel /Library/LaunchDaemons/com.alasia.plist
sudo chmod 644 /Library/LaunchDaemons/com.alasia.plist

# 加载服务
sudo launchctl load /Library/LaunchDaemons/com.alasia.plist

# 验证服务状态
sudo launchctl list | grep alasia
```

#### 4. 管理服务

```bash
# 停止服务
sudo launchctl unload /Library/LaunchDaemons/com.alasia.plist

# 重新加载（修改配置后）
sudo launchctl unload /Library/LaunchDaemons/com.alasia.plist
sudo launchctl load /Library/LaunchDaemons/com.alasia.plist

# 查看日志
tail -f /var/log/alasia.log
```

> **提示**：`StartInterval` 单位为秒，300 表示每 5 分钟执行一次。也可使用 `StartCalendarInterval` 实现类似 cron 的定时表达式。

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
    ├── cache.hpp / cache.cpp
    ├── ip_getter.hpp / ip_getter.cpp      # Linux netlink + HTTP API
    ├── ip_getter_bsd.cpp                  # macOS/FreeBSD/OpenBSD ioctl
    └── provider/
        ├── provider.hpp
        ├── cloudflare.hpp / cloudflare.cpp
        └── aliyun.hpp / aliyun.cpp
```

## 平台支持

| 平台 | IPv6 获取方式 | 状态 |
|------|--------------|------|
| Linux | netlink (`RTM_GETADDR`) | ✅ 完整支持 |
| FreeBSD | ioctl (`SIOCGIFALIFETIME_IN6`) | ✅ 支持 |
| OpenBSD | ioctl (`getifaddrs`) | ✅ 支持 |
| macOS | ioctl (`SIOCGIFALIFETIME_IN6`) | ⚠️ **实验性支持** |

> **macOS 说明**：macOS 支持目前为实验性质，作者无 Mac 设备进行测试。理论上代码可正常工作，但可能有未发现问题。欢迎 Mac 用户测试并反馈。

所有平台均支持 HTTP API 降级方式获取 IPv6 地址。

## 许可证

采用 **MIT License** - 详见 [LICENSE](LICENSE) 文件。
