# Gecko-ddns

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD-yellow)](README.md)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue)](README.md)

用 C++23 写的 IPv6 DDNS 客户端。从网卡或 HTTP 接口取出当前 IPv6，更新 Cloudflare 和阿里云的 AAAA 记录。

只支持 Linux 和 FreeBSD。

## 构建

依赖：CMake 3.20+、C++23 编译器、libcurl、OpenSSL。头文件库优先用系统里的 nlohmann-json 和 argparse，找不到再从 GitHub 下载。

```bash
./build.sh
./build/gecko-ddns version
```

FreeBSD 上建议先安装头文件库：

```bash
sudo pkg install nlohmann-json argparse
```

`./build-static.sh` 用 Docker 里的 Alpine 打静态链接的 Linux 二进制，需要本机有 Docker。

调试构建可以打开 AddressSanitizer 和 UBSan：`cmake -B build-asan -DENABLE_SANITIZERS=ON && cmake --build build-asan`。

## 配置

```bash
cp config.example.json config.json
```

不用的服务商整段删掉。凭证每个服务商只写一次。记录类型固定是 AAAA，不出现在配置里。

```json
{
  "interface": "vtnet0",
  "fallback_urls": [
    "https://ipv6.icanhazip.com",
    "https://6.ipw.cn"
  ],
  "proxy": "",
  "cloudflare": {
    "token": "Cloudflare API Token",
    "records": [
      {
        "name": "www.example.com",
        "zone_id": "",
        "ttl": 180,
        "proxied": false,
        "use_proxy": false
      },
      {
        "name": "example.com",
        "zone_id": "",
        "ttl": 180,
        "proxied": false,
        "use_proxy": false
      }
    ]
  },
  "aliyun": {
    "access_key_id": "AccessKey ID",
    "access_key_secret": "AccessKey Secret",
    "records": [
      {
        "name": "www.example.cn",
        "ttl": 600,
        "use_proxy": false
      }
    ]
  }
}
```

| 字段 | 含义 |
|------|------|
| `netns` | 可选，仅 Linux。留空则在宿主机上取地址。名字对应 `/run/netns/<名字>`，也可以写该目录下的绝对路径。非空时必须同时写 `interface` |
| `interface` | 网卡名。Linux 上例如 `eth0`，FreeBSD 上例如 `vtnet0`。和 `fallback_urls` 至少写一个。设了 `netns` 时必填 |
| `fallback_urls` | 网卡获取失败时按顺序尝试的 HTTP 地址。这些请求不走代理，也没有真实的地址生命周期 |
| `proxy` | DNS API 代理，支持 `http`、`https`、`socks5`、`socks5h`。留空表示不使用 |
| `cloudflare.token` | Cloudflare API Token，需要 `Zone:DNS:Edit` |
| `aliyun.access_key_id` / `access_key_secret` | 阿里云密钥，需要 `AliyunDNSFullAccess` |
| `records[].name` | 完整域名。`example.com` 是根域名，`www.example.com` 的主机记录是 `www` |
| `records[].zone_id` | 仅 Cloudflare。留空则查询后写入 `cache.zoneid.json`，不回写配置文件 |
| `records[].ttl` | 可选。Cloudflare 缺省 180，阿里云缺省 600 |
| `records[].proxied` | 仅 Cloudflare，是否开启橙云，默认 false |
| `records[].use_proxy` | 为 true 时，这条记录的 API 请求走全局 `proxy` |

`example.com.cn` 这种多段公共后缀的根域名，和它下面的主机无法从名字本身区分开。根域名请写成只有两段的名字，或者把主机写在最左边，例如 `www.example.com.cn`。

选出地址时丢掉链路本地、ULA、环回和已弃用地址。剩下的全局单播里，取 preferred lifetime 最长的。生命周期用内核原始的 pltime/vltime，和 `ifconfig` 不加 `-L` 时一致。`0xffffffff` 视为永久。

IP 没变则跳过更新。缓存文件 `cache.lastip` 只保留最后一次 IP，放在配置文件旁边；`-d` 可以改目录。旧的「时间 IP」多行缓存仍能读出最后一条。

## 运行

```bash
./build/gecko-ddns run -c config.json
./build/gecko-ddns run -c /etc/gecko-ddns/config.json -d /etc/gecko-ddns
```

| 参数 | 简写 | 默认 | 说明 |
|------|------|------|------|
| `--config` | `-c` | 无 | 配置文件 |
| `--dir` | `-d` | 配置文件所在目录 | 缓存目录 |
| `--ignore` | `-i` | false | 忽略缓存，强制更新，并且不写缓存 |
| `--timeout` | `-t` | 300 | 超时（秒） |
| `--netns` | | 配置文件里的 `netns` | 覆盖配置，只在 Linux 上探测这个 network namespace |

`version` 打印版本。

## 网络命名空间（仅 Linux）

程序仍在宿主机里跑。设了 `netns` 之后，只有取地址发生在那个 namespace 里：网卡枚举和 HTTP 回显都进去，Cloudflare 和阿里云的 API 留在宿主机。服务商 API 不依赖本机出口，而且 namespace 里可能没有外网。

```json
{
  "netns": "vpn",
  "interface": "vpn-mv",
  "fallback_urls": ["https://ipv6.icanhazip.com", "https://6.ipw.cn"]
}
```

`config.example.json` 里的 `"netns": ""` 保持宿主机行为。命令行 `--netns vpn` 覆盖配置文件。名字只允许字母、数字、`_`、`.`、`-`，最长 64，禁止 `..`。绝对路径规范化之后必须仍在 `/run/netns/` 下面。FreeBSD 上只要写了 `netns` 就会报错 `netns is only supported on Linux`。

需要 root，或者 `CAP_SYS_ADMIN`（`setns`）。不要给二进制加 setuid。

宿主机的 `/etc/resolv.conf` 经常指向 `127.0.0.53`。这个地址在目标 namespace 里是它自己的环回，没有人听，HTTP 回显的域名会解析失败。进入 namespace 之后会先 `unshare` 出一个新的挂载命名空间，再把 `/etc/netns/<名字>/resolv.conf` 绑到进程看到的 `/etc/resolv.conf`。没有这份文件时，改用可读的 `/usr/local/etc/resolv.conf`。两份都没有就在 stderr 警告并继续。`unshare` 失败时不会做绑定，避免改到宿主机的 resolv.conf。

探测失败不会退回宿主机的网卡或 HTTP 接口，避免把宿主机地址写进 DNS。失败时不写缓存、不更新记录。

| 退出码 | 含义 |
|--------|------|
| 10 | netns 不存在 |
| 11 | 权限不够，需要 root 或 `CAP_SYS_ADMIN` |
| 12 | namespace 里没有可用的全局 IPv6，API 兜底也失败 |
| 13 | 网卡不存在，而且 API 兜底失败 |
| 14 | 子进程输出无法解析，或地址不是全局单播 |

`__probe` 是内部子命令，stdout 只有一行 JSON。

### systemd（vpn namespace）

`/etc/systemd/system/gecko-ddns-vpn.service`：

```ini
[Unit]
Description=gecko-ddns for vpn netns
After=vpn.target
Wants=vpn.target

[Service]
Type=oneshot
User=root
ExecStart=/usr/local/bin/gecko-ddns run -c /etc/gecko/vpn.json --netns vpn
```

`/etc/systemd/system/gecko-ddns-vpn.timer`：

```ini
[Timer]
OnBootSec=2min
OnUnitActiveSec=5min
Persistent=true
```

排错：

- 退出码 10：`ip netns list` 里没有这个名字，或 `/run/netns/<名字>` 不存在。
- 退出码 11：用 root 跑，或给进程 `CAP_SYS_ADMIN`。
- 退出码 12：`ip netns exec vpn ip -6 addr show dev vpn-mv`。只有 ULA（`fc00::/7`）会被拒绝。
- API 兜底解析不了域名：准备 `/etc/netns/vpn/resolv.conf`。
- 对照地址：`ip netns exec vpn ip -6 -o addr show dev vpn-mv`。

### systemd

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
Description=Run Gecko-ddns every 10 minutes
Requires=gecko-ddns.service

[Timer]
OnBootSec=5min
OnUnitActiveSec=10min
Unit=gecko-ddns.service

[Install]
WantedBy=timers.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now gecko-ddns.timer
```

日志在 stdout。systemd 下用 `journalctl -u gecko-ddns.service` 查看。

## 许可

MIT。见 [LICENSE](LICENSE)。

依赖：[nlohmann/json](https://github.com/nlohmann/json)、[argparse](https://github.com/p-ranav/argparse)、[libcurl](https://curl.se/)、[OpenSSL](https://www.openssl.org/)。
