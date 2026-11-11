# rdpch — RDP 虚拟通道 HTTP/HTTPS 转发

让 RDP **服务器**(被控端)的 HTTP/HTTPS 流量经 RDP 静态虚拟通道转发到
**客户端**(控制端),由客户端实际向目标发起请求,响应原路返回。
效果等价于 "SSH -D over RDP",但载体是 RDP 自身的虚拟通道,无需在两端
另开端口、无需 VPN。

**适用场景**:服务器无法/不宜直接访问外网,而控制端可以 —— 服务器上的
浏览器与命令行工具借道你正在使用的这条 RDP 会话出网。

```
[服务器上的应用/浏览器] --HTTP 代理(127.0.0.1:18080)--> rdpchsrv.exe(服务器, RDP 会话内)
        9 字节自描述帧 over RDP 静态通道 "RDPCH"(WTSVirtualChannel* / mstsc AddIns)
                                    rdpchcli.dll(客户端, mstsc.exe 进程内)
        每连接一个 socket,DNS 解析在客户端 --> [目标站点 http:80 / https:443]
```

已在真实两机环境完成端到端验证(2026-09,Windows 11 26100 客户端 ↔
Windows Server 远端):curl HTTP/HTTPS、CONNECT 隧道、系统代理下的浏览器
流量均稳定通过,RDP 会话长时间在线无闪断。

## 组成

| 产物 | 位置 | 说明 |
|------|------|------|
| `rdpchcli.dll` | mstsc.exe 进程内 | 客户端插件(x64/x86 双版本),含中继核心 |
| `rdpchsrv.exe` | RDP 会话内 | 服务端:HTTP 代理 + WTS 通道传输 |
| `rdpchclihost.exe` | 任意控制台 | 客户端核心的 mock 宿主(无 RDP 全链路联调用) |

## 快速开始(真实两机)

1. **客户端(控制端,你面前的电脑)一次性安装**:

   ```
   scripts\install-client.cmd
   ```

   脚本把 `dist\x64\rdpchcli.dll` 拷到 `%LOCALAPPDATA%\rdpch\` 并写注册表。
   之后**新开** mstsc 连接即会加载插件(已运行的 mstsc 不会动态加载)。

2. **用 mstsc 正常登录服务器**(被控端)。

3. **服务器上启动**(在 RDP 会话内):

   ```
   rdpchsrv.exe --port 18080 --setproxy --bypass "192.168.*,10.*"
   ```

   `--setproxy` 会把当前用户的系统代理指向 `127.0.0.1:18080`,
   服务器上的浏览器/应用即刻走隧道;不需要时去掉该参数即可。
   `--bypass` 指定不走代理、由服务器本机直连的目标(见下文"代理例外"),
   可省略。

4. **验证**(服务器上):

   ```
   curl -x http://127.0.0.1:18080 http://example.com -v
   curl -x http://127.0.0.1:18080 https://example.com -v
   ```

卸载:客户端 `scripts\uninstall-client.cmd`;服务器 `rdpchsrv --unsetproxy`
还原系统代理后退出(手动 Ctrl-C 退出时也会自动还原)。

## 构建

需要 VS 2026(或任何带 Windows SDK 的 MSVC)+ CMake:

```
scripts\build.cmd
```

产物在 `dist\x64\`(全量)与 `dist\x86\`(仅 `rdpchcli.dll`)。
构建同时编译并运行单元测试(`test_protocol`、`test_http_rewrite`、`test_conf`、
`test_vc`、`test_backpressure`)。
无 RDP 环境还能跑全链路冒烟:`test\mock_e2e.cmd`(见下)。

## 部署细节

### 客户端(控制端)

以管理员或普通用户运行(写 HKCU,无需管理员):

```
scripts\install-client.cmd          :: 64 位 mstsc(默认)
scripts\install-client.cmd /x86     :: 32 位 mstsc
scripts\uninstall-client.cmd
```

> **注册表值名的重要发现**(2026-09 实测,procmon 铁证):较新的
> mstsc 构建(Windows 26100.8875 验证)在 `AddIns\<名>` 子键下实际查询的
> 值是 **`Name`**,而非旧文档记载的 `DllName`;缺少 `Name` 时插件被
> **静默跳过**(无任何报错、事件日志)。安装脚本因此同时写入
> `Name` + `DllName` 两个值以兼容新老构建。手工注册时务必两个都写。
> 排障手段:DLL 被加载时会在自身目录追加一行 `load_canary.txt`(含
> 加载进程 PID),可据此区分"未加载"与"加载后失败"。

> DLL 在 `DllMain` 自 pin 且 mstsc 运行期间锁定文件:**更新 DLL 前必须
> 先关闭所有 mstsc 窗口**。

### 服务器(被控端)

在 RDP 会话内运行:

```
rdpchsrv.exe                        :: 默认监听 127.0.0.1:18080
rdpchsrv.exe --port 18080 --max-conns 128
rdpchsrv.exe --setproxy             :: 起服务成功后再把系统代理指向本代理
rdpchsrv.exe --setproxy --bypass "192.168.*,10.*,*.corp.local"
rdpchsrv.exe --setproxy --config D:\conf\rdpchsrv.json
rdpchsrv.exe --unsetproxy           :: 恢复 --setproxy 之前的系统代理设置
```

`--setproxy` 旧值备份在 `HKCU\Software\rdpch\proxy-backup`,
`--unsetproxy` 精确还原;本机地址(`localhost;127.*;<local>`)始终在
绕过列表中;进程 Ctrl-C 退出时若曾 `--setproxy` 也会自动还原。
同会话防双实例(`Local\rdpchsrv` 互斥量)。

#### 代理例外(--bypass)

`--bypass` 追加 WinINet 例外列表(ProxyOverride)条目,命中的目标
**不经代理、由服务器本机直连**,常用于放行内网网段。条目用 `;` 或
`,` 分隔,匹配规则是 WinINet 的按名字符串匹配(不做 DNS 解析):

| 写法 | 命中对象 |
|---|---|
| `192.168.*` / `10.*` | 按 IP 直访时的对应网段 |
| `172.16.*`~`172.31.*` | 172.16/12 私有段(需逐条列出) |
| `*.corp.local` | 该域的子域名 |
| `<local>`(默认已含) | 不带点的主机名(NetBIOS 名) |

私有三段完整写法:

```
--bypass "192.168.*,10.*,172.16.*,172.17.*,172.18.*,172.19.*,172.20.*,172.21.*,172.22.*,172.23.*,172.24.*,172.25.*,172.26.*,172.27.*,172.28.*,172.29.*,172.30.*,172.31.*"
```

每次 `--setproxy` 都以备份的原始列表为基准重新计算(原始条目 → 本机
条目 → `--bypass`/配置文件条目,去重),因此改了或去掉 `--bypass` 后
重启即生效,不会残留旧条目。注意:用域名访问内网且域名解析到内网 IP
时,通配网段条目命中不了,需按域名写(`*.corp.local`)或依赖 `<local>`;
若需要严格按解析后 IP 判断,需在代理内部做 CIDR 直连(未实现)。

##### 配置文件(JSON)

bypass 条目可写在 JSON 配置文件里,`--config` 指定路径,默认读 **exe
同目录**的 `rdpchsrv.json`(不存在则忽略):

```json
{
  "log_level": "info",
  "bypass": [
    "192.168.*",
    "10.*",
    "*.corp.local"
  ]
}
```

- `"bypass"` 也可以是一个字符串(`"192.168.*,10.*"`);
- `"log_level"` 为 `debug`(默认)/`info`/`warn`/`error`,控制日志输出
  级别;环境变量 `RDPCH_LOG` 指定级别时**优先于**配置文件;
- 配置文件条目与命令行 `--bypass` 条目**合并生效**(注册时去重);
- 未知的键被忽略,文件可以放其他配置扩展;UTF-8(带不带 BOM 均可);
- 文件缺失是正常情况(返回"无配置"),JSON 语法错误会在日志里报
  `conf: malformed JSON config` 并按无 bypass 处理。

日志级别(环境变量方式)对客户端插件同样有效:`setx RDPCH_LOG warn`
后重开 mstsc;但配置文件只作用于 rdpchsrv(客户端拿不到 exe 路径)。

服务器侧可独立于客户端重启:通道断开时 `rdpchsrv` 以指数退避(1s→5s)
重开 `WTSVirtualChannelOpenEx`,重连后自动重握手(见下节),无需重启
mstsc。反向亦然 —— mstsc 断开重连后服务端通道自动恢复。

### 无 RDP 联调(mock 全链路)

```
test\mock_e2e.cmd
```

`rdpchsrv --mock-listen 19001` 用 TCP 监听代替 RDP 通道,`rdpchclihost
--connect 127.0.0.1:19001` 代替 mstsc 插件,其余代码路径完全一致。

## 协议

通道上是连续字节流(mstsc 的 DATA_RECEIVED 分块与服务端 WTS Read 的
返回都无帧语义),两端用同一个增量解析器还原帧:

```
帧 = conn_id(u32 LE) | cmd(u8) | length(u32 LE) | payload(0..1400B)
cmd: 1=HELLO 2=OPEN 3=OPEN_OK 4=OPEN_FAIL 5=DATA 6=CLOSE 7=PING 8=PONG 9=BYE
HELLO: ver(2B=1)+max_payload(2B)+reserved(2B),通道每次建立各发一次
OPEN:  port(2B)+hostlen(2B)+host(域名在客户端解析)
OPEN_FAIL reason: 1=DNS 2=REFUSED 3=TIMEOUT 4=LIMIT 5=PROTO
CLOSE reason: 0=EOF 1=ERROR 2=OVERFLOW
conn_id 由服务端从 1 递增分配,0 保留给控制帧
非法 cmd / length>1400 → 协议错误杀传输(流上无法重同步)
```

**握手与重同步语义**(传输层,两端一致):

- 载体建立时双方各发一次 HELLO;收到合法 HELLO 前,收到的其他帧
  **静默丢弃**(对端刚重启,旧连接状态已无处对账),不视为错误。
- 已建立的一端再次收到 HELLO(= 对端进程或载体重启过)→ 立即补发
  一次自己的 HELLO,把对端新链路的门打开;补发带 3 秒限速,防止
  双端连续闪断时互发 HELLO 打乒乓。
- 门未开的一方每秒重发一次 HELLO,直到对端应答。否则握手帧一旦丢失
  或被对端的补发限速吞掉(闪断风暴里两次重开相隔 < 3 秒即触发),
  对端毫无感知、照常应答,而本端把每个应答都按 pre-hello 丢弃 ——
  代理表现为所有请求 504,且永不自愈(2026-11 真实事故)。1 Hz 重发
  在对端限速窗口过后必然换来应答,数秒内自动恢复。
  **任一端独立重启都会自动恢复,不需要另一端做任何事。**
- 只有结构性损坏(非法 cmd/超长帧)才杀传输。

HTTP 侧:`CONNECT` 走隧道(等 OPEN_OK 才回 `200 Connection
established`);`http://` 绝对 URI 在服务端改写为 origin-form 后转发
(支持 keep-alive 流水线、chunked、HTTP/1.0 隐式体的状态机见
`src/srv/http_rewrite.c`);`https://` 绝对 URI 拒绝(502)——TLS 必须
走 CONNECT。

## 日志与排障

设环境变量 `RDPCH_LOG`(对 mstsc 内的 DLL 与 rdpchsrv 均生效):

- `debug|info|warn|error`:级别过滤(输出到调试器 OutputDebugString)
- 其他值被当作日志文件路径(追加写),如
  `set RDPCH_LOG=D:\temp\rdpch_dll.log`

控制台程序(rdpchsrv/rdpchclihost)始终镜像到 stderr。

常见问题速查:

| 症状 | 原因 | 处置 |
|------|------|------|
| 服务端 `WTSVirtualChannelOpenEx` 失败 err=50 | 客户端 mstsc 没加载插件,通道未协商 | 见上"注册表值名"注意事项;看 `%LOCALAPPDATA%\rdpch\load_canary.txt` 是否生成 |
| err=2/找不到通道 | 服务端先于 mstsc 握手启动 | 正常,1s→5s 退避自动重试 | 
| 更新 DLL 报文件占用 | mstsc 进程 pin 住了 DLL | 关闭所有 mstsc 窗口再拷贝 |
| curl 长时间无响应后 504 | OPEN 未收到 OPEN_OK(目标不可达/DNS) | 客户端看 RDPCH_LOG 里 `open` 行;确认目标在**控制端**可达 |

## 源码结构

```
src/common/   协议解析器、传输抽象与 HELLO 链路状态机、缓冲、日志、工具
src/srv/      rdpchsrv:WTS 通道 IO 线程、HTTP 代理(CONNECT/绝对 URI 改写)、系统代理设置
src/cli/      rdpchcli.dll:mstsc 入口(VirtualChannelEntry)、VC 传输泵、中继核心;rdpchclihost mock 宿主
test/         协议/改写器单元测试、mock 全链路脚本
scripts/      构建、客户端安装/卸载
```

并发要点:服务端一条 IO 线程独占 WTS 句柄(事件驱动读 + 出锁写);
客户端单飞写队列(pump 线程唯一写者,WRITE_COMPLETE 续传);连接层
每连接一线程 + 引用计数;任何线程不持锁做 socket/DNS/通道调用。

## 已知限制

- 仅 in-box `mstsc.exe` 支持 AddIns;Windows App 版远程桌面 / MSRDC
  不支持经典静态通道插件。
- Addins 注册键 32/64 位共享:同一时刻只能注册一种位数的 DLL
  (默认 x64;需要 32 位时 `install-client.cmd /x86` 重装)。
- 只隧道**当前用户 RDP 会话内**显式配置了该代理的流量;FTP、h2c、
  非浏览器流量不经过系统代理。
- 每连接背压上限 4MB、总计 128 连接,超限主动断开(OVERFLOW)。
- **流控**:socket→通道方向按传输层排队积压节流(高水位 1MB),RDP 链路慢时
  经 TCP 背压回传给浏览器/源站——大响应、大 POST 只会变慢,不会中断。
- 半关闭:CLOSE(EOF) 是 ssh -L 语义的上游结束——对端排空缓冲、半关闭目标、
  继续转发目标剩余输出,目标关闭才回 CLOSE(EOF);最终排空仍受 5 秒预算约束,
  极慢的接收端存在最多 5 秒的截断边界。
