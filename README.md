# ESP32-S3 USB 有线网 → Wi-Fi 共享

把电脑的有线网络通过 ESP32-S3-N16R8 的 USB 转成 Wi-Fi 热点，并提供网页管理。

连上热点后打开 **http://192.168.4.1** 可以改 Wi-Fi、USB 协议，以及查看/屏蔽已连接设备。

板子做 NAT 路由：USB 侧向电脑拿地址（ICS 一般为 `192.168.137.x`），热点侧固定 `192.168.4.1`。

## 首次烧录（两套 USB 固件）

Flash 里有两个应用分区：`ota_0` = RNDIS，`ota_1` = NCM。网页切换协议会改启动分区并重启，不必再重新编译。

在已 `export` 的 ESP-IDF 环境中：

```bat
python tools/dual_fw.py all -p COM40
```

只编译：

```bat
python tools/dual_fw.py build
```

分区表从「单应用」改成「双 OTA」后，建议整片烧录一次（上面的 `all` 会写 bootloader、分区表、两套固件）。

IDE 里单独 `idf.py flash` 只会更新当前/默认的 RNDIS 槽。换 NCM 仍要用 dual 脚本先把 `ota_1` 写进去。

## 网页管理

1. 手机连接热点（默认 `ESP32-S3-Share` / `12345678`）
2. 打开 `http://192.168.4.1`（也可用 `http://esp32-share.local`）
3. 改完后点页面底部「保存设置」才发给板子；改 USB 协议会重启。踢下线/屏蔽会马上生效。
4. 屏蔽名单会立刻把该设备踢下线并拒绝再次接入

## USB 协议

| 选项 | Windows 表现 | 驱动 |
|------|----------------|------|
| **RNDIS**（默认） | `Remote NDIS based Internet Sharing Device` | 一般免驱 |
| **NCM** | `CDC NCM` / `USB net` | 手动绑定 Microsoft **UsbNcm Host Device** |

切换协议后请先在设备管理器里卸载旧的 USB 网卡，再重新插拔。

## 硬件

- 板子：ESP32-S3 **N16R8**（16MB Flash + 8MB 八线 PSRAM）
- 必须用 **原生 USB**（GPIO19 D- / GPIO20 D+），不是 CH340/CP2102 那个串口
- 双 Type-C 板：插标了 `USB` 的口，不要插 `COM` / `UART`
- 日志在 UART0（GPIO43/44）

## 默认热点

- SSID：`ESP32-S3-Share`
- 密码：`12345678`
- 信道：6
- 管理页：`http://192.168.4.1`

## RNDIS（默认，Windows 10 免驱）

插上原生 USB 后，网络适配器中应出现 `Remote NDIS based Internet Sharing Device`。若在「其他设备」：右键 → 更新驱动程序 → 浏览我的电脑 → 让我从列表中选取 → **网络适配器** → **Microsoft** → **Remote NDIS Compatible Device**。

## NCM（手动绑定 UsbNcm）

1. 网页选 NCM 并重启（需已 dual flash）
2. 设备管理器里找到带感叹号的 **CDC NCM** / **USB net**
3. 更新驱动程序 → 从列表选择 → **网络适配器** → **Microsoft** → **UsbNcm Host Device**

## Windows 共享有线网

1. USB 网卡出现在网络适配器里
2. 右键 **以太网** → 属性 → 共享
3. 勾选「允许其他网络用户通过此计算机的 Internet 连接来连接」
4. 「家庭网络连接」选中这块 USB 网卡
5. 板子从 ICS 拿 `192.168.137.x`，再给热点分配 `192.168.4.x`

## 源码结构

```
main/
  main.c                 启动接线：NVS → USB WAN → SoftAP → NAT → Web
  config/                NVS 配置
  boot/                  双 OTA 槽切换（RNDIS ota_0 / NCM ota_1）
  wifi/                  SoftAP、踢线、DHCP 记录
  net/                   USB WAN 网卡、NAPT
  web/                   管理页与 API
  usb/
    net_class.c          TinyUSB 网络类回调
    usb_desc.h           描述符填充接口
    rndis/               RNDIS 描述符
    ncm/                 NCM 描述符
    tusb_cfg/            TinyUSB 编译开关
```
