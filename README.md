# ESP32-S3 USB 有线网 ↔ Wi-Fi



两种**互斥、互相独立**的工作模式（网页里切换，会重启）。不要混用 Windows「Internet 连接共享」。



| 模式 | 电脑 USB | 板子 Wi-Fi | Windows 要做的 |

|------|----------|------------|----------------|

| **共享热点** | 向电脑拿地址（ICS 一般为 `192.168.137.x`） | 开 SoftAP，`192.168.4.1` | 把有线网上网共享给 USB 网卡 |

| **USB 网卡** | 板子当 DHCP 服务器，`192.168.5.x` | **只做 STA**，连家里 2.4G Wi-Fi，**不开热点** | 关掉共享；USB 网卡自动获取 IP；打开 `http://192.168.5.1` 去连 Wi-Fi |



USB 网卡模式用单独的 USB 产品 ID / 序列号，Windows 会把它当成**另一块网卡**，不会把共享热点那次开的 ICS 带过来。



## 首次烧录（两套 USB 固件）



Flash 里有两个应用分区：`ota_0` = RNDIS，`ota_1` = NCM。网页切换协议会改启动分区并重启。



```bat

python tools/dual_fw.py all -p COM40

```



只编译：



```bat

python tools/dual_fw.py build

```



分区表从「单应用」改成「双 OTA」后，建议整片烧录一次（`all` 会写 bootloader、分区表、两套固件）。



IDE 里单独 `idf.py flash` 只会更新当前/默认的 RNDIS 槽。换 NCM 仍要用 dual 脚本先把 `ota_1` 写进去。



## 网页管理



1. **共享热点**：手机连热点（默认 `ESP32-S3-Share` / `12345678`），打开 `http://192.168.4.1`

2. **USB 网卡**：电脑浏览器打开 `http://192.168.5.1`（不要再用 192.168.4.1）

3. 也可用 `http://esp32-share.local`

4. 改协议或工作模式后点底部「保存设置」会重启。连家里 Wi-Fi 用网卡页上的连接/断开，不必点保存。



## USB 网卡模式



这不是 Windows 右下角 Wi-Fi 图标里的无线网卡。USB 只有 RNDIS/NCM，系统把它当成**有线网卡**。家里的 Wi-Fi 在板子网页上扫描、连接。



**USB 网卡模式请用 NCM。** RNDIS 在持续转发时 Windows 可能卸掉网卡；NCM 没有这套心跳，更稳。共享热点仍可用 RNDIS（免驱）。
>NCM 能稳住、RNDIS 会消失，是因为 Windows 对这两种协议的容错完全不同，不是网速匹配的问题。
RNDIS 大约每 5 秒发一次 KEEPALIVE，设备必须在 EP0 上回复，再通过中断端点通知主机去取。这个过程和数据包走的是同一个 TinyUSB 任务。NCM 没有这套心跳，USB 任务卡一会儿只是掉几个包；RNDIS 错过几次 keepalive，Windows 就把网卡卸掉，表现就是「网卡消失」。
RNDIS 还更吃事件队列：每个以太网帧就是一次 USB 传输，IN 端点忙着时我们仍会往 TinyUSB 队列里塞发送请求。NCM 可以把多帧打进一个 NTB，队列压力小得多。



步骤：



1. 先在共享热点模式打开管理页，工作模式选「USB 网卡」，协议选 **NCM**，保存并等重启（板子热点会关掉）

2. 电脑把这块 **新出现的** USB 网卡设为自动获取 IP，应拿到 `192.168.5.x`，网关 `192.168.5.1`

3. 打开 **http://192.168.5.1**，扫描后点连接

4. ESP32-S3 **只能连 2.4GHz**

5. **不要**对该网卡开启 Windows Internet 连接共享

6. 家里热点若是无密码，必须勾选「开放网络」再连接



USB 是全速 12 Mbps，实际大约 6–12 Mbps，跟不上家里宽带是硬件限制。



切回共享热点：在 `http://192.168.5.1` 改回「共享热点」并保存，再用手机连板子热点。



## USB 协议



| 选项 | Windows 表现 | 驱动 |

|------|----------------|------|

| **RNDIS** | `Remote NDIS based Internet Sharing Device` | 一般免驱。适合共享热点；USB 网卡模式不推荐 |

| **NCM**（USB 网卡推荐） | `CDC NCM` / `USB net` | 手动绑定 Microsoft **UsbNcm Host Device** |



切换协议或工作模式后，请在设备管理器里卸载旧的 USB 网卡再插拔一次。



## 硬件



- 板子：ESP32-S3 **N16R8**（16MB Flash + 8MB 八线 PSRAM）

- 必须用 **原生 USB**（GPIO19 D- / GPIO20 D+），不是 CH340/CP2102 那个串口

- 双 Type-C 板：插标了 `USB` 的口，不要插 `COM` / `UART`

- 日志在 UART0（GPIO43/44）



## 默认热点（仅共享热点模式）



- SSID：`ESP32-S3-Share`

- 密码：`12345678`

- 信道：6

- 管理页：`http://192.168.4.1`



## RNDIS（默认，Windows 10 免驱）



插上原生 USB 后，网络适配器中应出现 `Remote NDIS based Internet Sharing Device`。若在「其他设备」：右键 → 更新驱动程序 → 浏览我的电脑 → 让我从列表中选取 → **网络适配器** → **Microsoft** → **Remote NDIS Compatible Device**。



## NCM（USB 网卡推荐，需手动绑定 UsbNcm）



1. 网页选 NCM 并重启（需已 dual flash）

2. 设备管理器里找到带感叹号的 **CDC NCM** / **USB net**

3. 更新驱动程序 → 从列表选择 → **网络适配器** → **Microsoft** → **UsbNcm Host Device**



## Windows 共享有线网（仅共享热点模式）



1. USB 网卡出现在网络适配器里

2. 右键 **以太网** → 属性 → 共享

3. 勾选「允许其他网络用户通过此计算机的 Internet 连接来连接」

4. 「家庭网络连接」选中这块 USB 网卡

5. 板子从 ICS 拿 `192.168.137.x`，再给热点分配 `192.168.4.x`



USB 网卡模式下不要开 ICS。



## 源码结构



```

main/

  main.c                 启动：共享=USB WAN+SoftAP；网卡=USB LAN+STA

  config/                NVS 配置

  boot/                  双 OTA 槽切换（RNDIS ota_0 / NCM ota_1）

  wifi/                  SoftAP（仅共享）、STA、扫描

  net/                   USB WAN / USB LAN、NAPT

  web/                   管理页与 API

  usb/

    net_class.c          TinyUSB 网络类回调

    usb_desc.h           描述符（共享与网卡使用不同 PID）

    rndis/               RNDIS 描述符

    ncm/                 NCM 描述符

    tusb_cfg/            TinyUSB 编译开关

```

