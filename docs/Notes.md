# 代码里删掉的"为什么"——集中记在这里

这份文档收的是源码里原本以注释形式存在、但按"只保留函数说明与函数内步骤注释"的规则
清理掉的**约束与踩坑记录**。它们不是背景故事，而是"改错了会静默出问题"的东西，所以
从代码里搬到这里集中保存。改到相关代码前先看一眼。

## 平台与编译器

- **`winsock2.h` 必须先于 `windows.h` 引入**（`src/control/ControlInternal.h`）。
  Windows 下反了会和 `windows.h` 里的 winsock1 冲突。
- **`SimEvp.h` 与 `<openssl/evp.h>` 互斥**（`include/utils/sim/SimEvp.h`）。
  该头文件是 `evp.h` 的替代品，不能与它同时包含；libcurl 仍链 libcrypto 时链接层面
  不冲突，靠的是头文件里的名称重映射。
- **枚举成员名要避开 Windows 宏**（`src/clients/dialer/DialerInternal.h` 的 `WaitResult`）。
  `windows.h` 定义了 `WAIT_FAILED`（`((DWORD)0xFFFFFFFF)`）等同名宏，而该模块会经
  `NetClient.h` → `curl.h` → `winsock2.h` 把 `windows.h` 带进来，成员名撞上就会被宏替换掉，
  报 `expected identifier before '(' token`。Linux 上不出现，只在 Windows 构建时炸。
- **线程间共享状态刻意只用 32 位量**（`src/utils/Watchdog.c`）。
  OpenWrt 路由器多是 32 位 MIPS，64 位量跨线程读写会被撕裂，可能算出错误超时。
  写入顺序是"先写预算、再自增序号"；读侧先读序号、再读预算、最后确认序号没变。

## 配置

- **默认参数必须与 `esurfingclient/files/etc/config/esurfingclient` 及 `s_default_cfg` 三处一致**
  （`src/config/ConfigDefaults.c`）。缺哪个参数，用户下次打开配置文件就会看到它被补上。
- **`s_cfg_log_dir` 只存配置里的原样文本**（定义在 `src/utils/LogCore.c`，声明在 `src/utils/LoggerInternal.h`）。
  它是给页面回显用的，不能回显解析后的绝对路径 —— 否则用户改一次配置就被写成一长串路径。

## 日志

- **OpenWrt 默认日志目录选 `/var/log/esurfing`**（`src/utils/LoggerDir.c`）。
  因为 `/var/log` 是 tmpfs：重启即清、不磨闪存、小容量设备不会被日志占满。
  配置里的 `log_dir` 同样生效（想把日志放 U 盘时用）。
  ⚠️ `files/etc/init.d/esurfingclient` 与 LuCI 的日志页都拿这个值兜底，改要一起改。
- **单条日志必须只用一次 `write` 写出**（`src/utils/LogCore.c`，`LOG_LINE_MAX` + 静态断言）。
  多进程共用同一个 `run.log`，分多次写会让不同进程的行互相穿插。
- **查询模式（`--print-log-dir` / `--list-accounts`）一行都不落盘，改写到 stderr**
  （`src/utils/LogControl.c` 里的 `set_logger_query_mode` + `LogCore.c` 的 `s_query_mode`）。这两个模式必须调 `load_cfg()` 才知道答案，而解析配置一定会写日志；
  那几行落进 `run.log` 后，OpenWrt 的 init 脚本会把这文件当成上一轮运行的日志归档走
  （它的判断就是"`run.log` 非空就归档"）—— 没有旧日志时凭空多出一个只有查询输出的 `.log`，
  有旧日志时归档里混进这几行。之所以是"改写 stderr"而不是"什么都不写"：配置有问题时得让人
  看见原因；stdout 要留给路径/账号列表，而 init 脚本调用这两个查询时都带了 `2>/dev/null`。
  查询模式比日志等级更硬：`load_cfg()` 会用配置里的 `log_lv` 调 `set_logger_level()`，
  查询模式必须在那之后依然生效。
- **OpenWrt 上的归档由 init 脚本负责，时机是"这一轮运行结束之后"**
  （`files/etc/init.d/esurfingclient` 的 `archive_previous_log`，挂在 `service_stopped` 上）。
  `run.log` 是多进程共用的，改名只能由一方来做，而 AUTH / WEB 角色在 `clean_logger()` 里刻意
  不改名（交给监管进程）；OpenWrt 上 procd 只跑 `--role auth`，没有监管进程，所以由脚本充当。
  四点容易踩：
  - **别在 `stop_service` 里自己 `kill` 实例**。procd 分得清"服务被停"和"实例自己没了"：
    脚本抢先把进程杀掉，procd 会把它当成实例意外退出，按 `respawn 60 5 5` 排一次 5 秒后的重新
    拉起；这一次到底会不会被拉起来，取决于紧接着的 `procd_kill` 有没有赶在 5 秒内把服务删掉
    —— 也就是取决于实例退得快不快，"有时候会多重启一次"就是这么来的（实测：绕开脚本直接
    `kill` 认证进程，procd 5 秒后确实拉起了新实例）。停止交给 `procd_kill`，它会把实例标记成
    正在停止，不触发 respawn，跟退出快慢无关。
  - 归档要等实例真的退出再改名：`service_stopped` 是 rc.common 在 `procd_kill` **之后**才调的
    钩子（上游 `stop()` 的顺序是 `stop_service` -> `procd_kill` -> `service_stopped`），
    在这里等实例消失再改名，才不会给一个还在写的 `run.log` 改名、把新日志卷进归档。
  - `procd_set_param term_timeout 15` 是配套的：procd 默认只等 5 秒就 SIGKILL，而登出请求
    可能慢过 5 秒，被砍掉的话账号会挂在服务端在线（这一条补上了以前脚本自己等 15 秒的效果）。
  - 安装/升级只重启一次：opkg 升级时 `prerm` 停旧实例、`postinst` 起新实例（都是 base-files 的
    `default_prerm` / `default_postinst` 代劳），包自己的 `postinst` 因此**不能**再停/起服务。
    实测留一条：包脚本里那次 `restart`/`reload` 会让 init 脚本在一次安装里打印**两次**
    "已启动 N 个认证进程实例"，外加一次注定失败的 `ubus call service delete ... (Not found)`。

## 智能多拨 (仅 OpenWrt 认证进程)

- **一个开关**: `smart_dial`。原先拆成 `demand_dial` + `smart_dial` 两个键且必须同时打开, 现在合并为一个 —— 智能多播本身就包含按需多拨的行为。老配置里只有 `demand_dial` 时, `complete_cfg` 会把值搬进 `smart_dial` 再删掉旧键。
- **所有内部时间用单调钟** (`get_steady_tm_ms`, CLOCK_MONOTONIC): 状态新鲜度、空闲计时、静默期、退避都是减法测间隔, 墙钟会被 OpenWrt 开机后的 NTP 校时整体前跳, 跳变会被当成"对端已死/空闲已满"。单调钟在内核里全局一致, 跨进程比较没问题; `/var/run` 是 tmpfs, 重启即清空。
- **认证错峰** (`auth_stagger_ms`, 默认 15000): 多个账号同时被拉起时按"排位-已在线数"档错开。校园门户会把同账号的并发登录互相踢掉 —— 重启时所有账号同一秒一起认证, 表现就是反复掉线。触发准入的那次调用放行, 紧接着的轮询在期限前拒绝。

- **状态文件在 `/var/run/esurfingclient/account<配置序号>.state`** (`src/utils/DemandDial.c`)。
  一行四个字段: `pid state since_ms updated_ms`。`state` 是 0 时段外 / 1 待机 / 2 认证中 / 3 在线。
  `since_ms` 只在状态变化时更新, `updated_ms` 每秒刷新。写入用临时文件加 `rename`。
  init 脚本在拉起实例之前、以及停止之后会删掉整个目录; 进程正常退出时 `shut()` 删自己那一个文件。
- **排位是「可用账号」的顺序, 不是配置文件里的下标** (`g_account_order`, 在 `load_cfg` 里、
  `--account` 挑走单个账号之前填好)。自动标记值也是按这个顺序算的, 所以排位必须和它用同一轮遍历。
- **只在 `--role auth` 且可用账号不少于 2 个时生效**。单进程模式、桌面端、只配了一个账号, 都按原来的方式认证。
- **看门狗会在 `dialer_app` 返回时关掉** (`DialerClient.c`)。认证进程如果还要继续 (重新认证或转入待机),
  `work_auth` 必须再 `watchdog_start()` 一次, 否则待机那几小时没有看门狗。
- **未认证线路的转发改写不碰 OUTPUT** (`DemandDial.c` 的 nft 表 `esurf_demand`, 钩子是 `prerouting` / `mangle + 1`)。
  认证进程的包走 OUTPUT 加 `SO_MARK`, 如果连 OUTPUT 一起改, 待机账号就无法再认证。
  标记掩码读 `/etc/config/mwan3` 的 `mmx_mask`, 读不到用 `0x3f00`。服务停止时 init 脚本删掉这张表。
  只有排在最前且状态文件还新鲜的那个认证进程会改这张表。每个进程各记一份「已经应用的规则」再一起写, 后写的会把刚上线的线路又改回第一条。
  **从"不是写入者"变回写入者时必须强制重下一遍** (`demand_steer` 里的 `applied_leader`): 每个进程的"已下发"缓存是各自记的, 轮到自己写的时候如果拿旧缓存一比"没变化"就跳过, 实际内核里的规则可能还是上一个写入者留下的另一套。

## 智能多播的判定与转发细节

- **重设备只能有一个统计者** (`DemandDial.c` 的 `heavy.state`)。它是"哪台设备下载最猛", 而每个进程的采样时刻不同, 各算各的就会得出不同的结果; 转发规则又是整套换掉的, 两个进程各按自己的结果下发, 规则就会在两个结果之间来回改。所以只让**排在最前面的那条在线线路**统计并写文件, 其它进程只读。文件里带 pid, 读侧要同时校验 pid 还活着和 `updated_ms` 没过期 (统计者被强杀时不能继续用它的结论)。
- **nft 的 `add table` / `add set` / `add chain` 是幂等的, `create` 才是"已存在就报错"** (`steer_apply_nft`)。所以转发规则可以写成"一个事务里 `add table/set/chain` + `flush chain` + `add rule`", 不必再走老版本的"`delete table` 再重建": 删表会把计数集合里每个元素的计数器一起清掉, 而设备速率正是靠前后两次采样的差值算出来的, 清一次就丢一个采样窗口。
- **`flush chain` 不会清集合元素的计数器**, 只有 `flush set` 会。`lan_nets` (局域网网段) 每次重下都 `flush set` 再填, 因为它没有计数器; `dev_dl` / `dev_ul` 绝对不能在重下规则时清。
- **连接数与设备数必须要有"活动门槛"** (总下行 + 总上行 ≥ 下行阈值的 1/4), 而且**收回判定也要受同一个门槛约束**。否则家里十几台设备一直挂着, 空载时设备数永远高于收回线的一半, 多开出来的线路就再也收不回去 —— 那和"平时只留一条"正好相反。
- **设备级独占的规则只动"局域网 → 外网"的转发** (`ip daddr != @lan_nets`)。局域网地址是内网, 打上 mwan3 的 fwmark 之后会被 `ip rule fwmark ... lookup <wanN>` 送到 WAN 表里去, 内网互访就断了。另外**不碰 OUTPUT**: 认证进程自己发的包在 output 链上, 连 OUTPUT 一起改的话待机账号就没法再认证了。
- **扩容、收回、设备级独占增删均不全表清理连接跟踪**。OpenWrt 的 `/proc/net/nf_conntrack` 写 `f` 是全表删除, 会连其它在线 WAN、管理连接和 IPv6 一起影响; 删除记录也不等于应用自动重连。扩容只影响后续新会话, 既有会话自然结束。
- **仅在 NAT 绑定前选择新连接的 WAN**, 同时保存最终 `ct mark` (`ct state new ct direction original ct status and confirmed == 0`)。只限制 `ct state new` 仍不够: 未收到回复的 UDP/TCP 重发可能一直是 NEW, 但已经 confirmed 并绑定了旧 NAT。旧版本独占规则对每个包改 `meta mark`, 又没有保存最终 `ct mark`, 会造成路由出口与 NAT 源地址错配; 原子下发只消除规则空窗, 不解决这个问题。
- **逐条收回与无损排空是两件事**: 当前只允许尾部仍在线的账号收回, 前面的账号等尾线退出、共享静默期结束后重新累计空闲时间。尚未实现按 WAN 会话的 DRAIN/排空; 退网线路上的长连接仍可能中断, 健康线路上的连接不应被主动迁移。
- **每设备统计只在有 nft 时可用**。当前 iptables 路径只做线路级改写 (`steer_apply_iptables`), 不做设备级独占 —— 扩容与收回照常, 只是没有独占。iptables 降级用 MARK + CONNMARK 保存最终分类, 同样只匹配 NEW/original/非 confirmed; 这条路径仍逐条重建规则, 不具备 nft 单事务的原子性。
- **`nft -j list set` 要 fork 一次外部命令**, 所以只有"排在最前面的在线线路"在"线路 ≥ 2 条"时才采, 而且固定 5 秒一次 (`SMART_ACCT_SAMPLE_MS`)。一条线路在线时不做每设备采样, 第二条线起来后需先建立累计计数基准, 再连续两次确认重设备 (通常约 10–15 秒)。
- **规则独立于重设备采样每 30 秒重下** (`DEMAND_REFRESH_MS`), 保留设备计数集、刷新 LAN 网段, 不打印重复状态日志。只有一条线路或关闭 `smart_dial` 时也要补规则, 否则外部删表后会永久命中本地缓存。计数读取失败/网口集合变化另设 dirty 提前补下。
- **准入/收回查询没有静默期副作用**。`s_last_change_ms` 在实际状态转换时更新; `in_hold_off` 同时读所有存活账号共享的 `since_ms`, 心跳只刷新 `updated_ms`。否则第二条线刚认证完成, 第三条进程的本地时钟没有记录, 会立刻再扩容。
- **采样不可用不是零流量**。缺失/过期统计暂停负载扩容和空闲收回并复位低负载计时; 网口集合变化重建累计字节基准, 防止新增网口的历史字节被当成突发。
- **被压回第一条在线线路的设备, 在那条线路真的断掉时会短暂受影响**: 线路断了但认证进程还没被判死之前, 规则仍指着它。恢复靠的是"那条线路的认证进程心跳失败退出 -> 状态文件过期 (`fresh_ms()`) -> 在线集合变化 -> 规则重下", 通常 5-20 秒。这是"只给重设备多路"这条需求本身的代价; 重设备自己不受影响 (它仍然由 mwan3 按健康状态分流)。
- **重设备状态里的地址必须再校验一遍** (`heavy_device`): 它会被直接拼进 nft 规则, 一旦不是合法 IPv4, `nft -f` 会把**整批**规则拒掉 —— 连线路改写一起失效, 而线路改写是原有功能。存档是原子写的 (临时文件 + rename), 正常不会坏, 但校验只有三行, 兜住这类"一个字段坏掉全盘失效"的风险。
- **下发失败要退避**: 失败时保留旧的成功缓存, 用完整目标标记、离线标记数组、智能开关和重设备作为失败键。相同内容 30 秒内暂停重试, 内容变化立即重试; 仅比较离线标记数量会漏掉 `{0x200}` → `{0x300}` 这种状态变化。
- **智能多播的集合与计数链是"总是创建"的**: 即使 `smart_dial` 关着, 规则文件里也会带上 `lan_nets` / `dev_dl` / `dev_ul` 与空的 `acct` 链。这样开关智能多播时不需要删链 (删不存在的链会让整批规则一起失败), 代价只是一个空链挂在 forward 钩子上, 没有实际开销。
- **`collect_lan_nets()` 靠"不是 WAN 口、不是点对点、不是 loopback"来认局域网**: 如果某条 WAN 在 mwan3 下没有默认路由 (于是不在 `demand_iface` 的自动列表里), 它的网段可能被当成局域网。可能污染设备统计或错误排除目的网段; 多拨环境建议把 `demand_iface` 显式写全, 并检查实际 `lan_nets` 集合, 不作无影响保证。
- **统计者换人时重设备会短暂消失**: `s_dev_prev` 是进程本地的, 原来的统计者退出后, 新的统计者要重新采两次才有速率 (约 10-15 秒), 这段时间不做设备级独占。独占规则本身不会因此出错, 只是暂时退回"全部交给 mwan3"。
- **重设备判定要有迟滞**: 连续 2 次采样都是同一个 IP 才算数, 掉到阈值一半以下连续 3 次才取消, 换设备还要求新候选比当前重设备快 1.5 倍。
- **mwan3 sticky 与单设备叠加有冲突**: stock mwan3 用 `hash:ip,mark` / `src,src` 绑定源设备与 WAN。要给一台设备按连接均衡, 匹配规则须 `sticky=0`, 成员 metric 相同; 客户端仅让重设备绕过 pin, 不主动管理 sticky 集合。参考 [mwan3 24.10 源码](https://raw.githubusercontent.com/openwrt/packages/openwrt-24.10/net/mwan3/files/lib/mwan3/mwan3.sh)。
- **设备计数要求管理流量经过经典转发路径**: 软件/硬件 flow offload 命中后绕过 prerouting/forward, 本模块的计数会少计。开启智能多播前关闭 offload 或由部署方排除受管理流量; 当前代码没有自动修改 fw4/PPE 设置。参考 [内核 flowtable 文档](https://docs.kernel.org/networking/nf_flowtable.html)。
- **测试口径**: 决策回归和 nft 语法/幂等校验之外, 还需真实双出口转发/NAT 抓包。qemu 运行产物或能列出规则不证明 mwan3 吞吐、sticky、offload 和退网排空正确。Linux 下执行 `sh scripts/test-demand.sh --network`: 决策/缓存/URL 测试带 ASan+UBSan, nft 在独立 user/net namespace 内实际加载两次; 双 WAN 测试模拟 mwan3 的 restore/classify/save 次序, 抓包覆盖既有会话、未回复 confirmed 会话、新 pin、取消 pin、离线新连接改写与删除表恢复。不是路由器实机测试。

## 看门狗

- **为什么需要它**（`include/utils/Watchdog.h`）：外部监管者（procd / systemd / SCM）只能看到
  "进程退出了没有"。进程卡死（死锁、无超时阻塞调用、绕不出来的死循环）时它照样活着，
  监管者认为一切正常、永远不会重启。OpenWrt 上尤其明显：那边连监管进程都没有，只有 procd
  在看进程在不在。
- **绝不能做成"多久没打日志就重启"**：本程序有大量合法的长时间静默（例如"不在允许时段，
  等待 3600000 毫秒后重新检查"），那样会误杀正常等待。判定依据是主循环**主动声明**
  （`watchdog_pet(30000)` = 我活着，接下来最多 30 秒不会再来打卡），只有超过声明上限加余量
  仍未打卡才判卡死。
- **判定卡死后 `_exit(1)`，不能调 `shut()`**：`shut()` 会 join 线程，可能正好卡在同一个死锁上；
  退出后交给外部监管者按 respawn 策略拉起。
- **"当前线程是不是看门狗自己"这个标记必须有**（`src/utils/Watchdog.c`，线程局部变量）：
  看门狗线程自己也要睡眠，而 `sleep_ms()` 会按睡眠时长打卡 —— 不排除自己，就等于看门狗给自己
  打卡、序号每个 tick 都在变，"有新打卡"分支永远成立，`watchdog_fire` 成了不可达代码。
  表现是"装了看门狗但卡死依然没人管"，且不报任何错。用线程局部量是因为只有本线程该被排除。

## 进程与控制通道

- **控制通道协议**（`src/control/ControlInternal.h`）：

  | 请求（一行 JSON） | 响应（一行 JSON） |
  |---|---|
  | `{"cmd":"status"}` | `{"ok":true,"data":{...}}` |
  | `{"cmd":"restart_auth"}` | `{"ok":true}` |
  | `{"cmd":"apply_config"}` | `{"ok":true}` |
  | `{"cmd":"shutdown"}` | `{"ok":true}` |

  进程拆分后 Web 进程既看不到认证状态、也不能直接改认证线程的运行时状态，所以要这条只监听
  回环的控制通道，鉴权靠监管者下发的令牌。**刻意不放配置读写**：两个进程读同一份配置，
  Web 进程保存配置也直接写文件，通道只负责运行时状态与动作，协议面最小、最不容易出错。
- **退出请求只由信号处理函数置位**（`src/supervisor/SupervisorInternal.h` 的 `s_stop_requested`）：
  信号处理函数里不能做 join / 打日志 / rename / exit 这些不是 async-signal-safe 的事，
  真正的关闭动作由各角色的主循环来做。
- **模块内共享状态的定义只放一处，头文件里只放 `extern` 声明**
  （`src/supervisor/SupervisorState.c`、`src/utils/LogCore.c` 等）。若把可变状态定义在头文件里，每个 `.c` 会拿到
  自己的一份副本，`s_child_count` 之类的计数就各算各的，行为与拆分前不同。
- **`_Thread_local` 的限定符在声明与定义两侧都必须带**（`src/clients/net/NetClient.c` 的
  `s_request_url`，声明在 `NetClientInternal.h`）。少了它就不是线程局部对象，`header_cb` 拼 Location
  时的基准地址会串到别的线程去。

## iOS ZSM 包格式（逆向参考）

`src/cipher/algo/ios/ios_zsm.c` 解析的 `IZsmModLoad`（`sub_10007131C`）布局：

```
[3-byte hdr][u8 len1][str1][u8 len2][str2=AID]
[5-byte LZMA props][u32le packed: top nibble type==2, low 28 bits unpacked size]
[TEA ciphertext...]
```

- TEA key：32 字节 ASCII，两段 16 字节，各 32 轮解密，8 字节一组
  （`Rirn53a;feb#UXES5ZrRBTGmYwml:fRt`）
- LZMA 解压后的布局：`buf[0xFA]` = IV 长度，`buf[0xFC]` = key 长度，
  `buf[0xFF]` = key 偏移基址（key 在 `buf[buf[0xFF]+1]`），JS 源码在 `buf+0x103`
- JS 全局量：`cdckey` / `cdciv` / `cdy(type, mode, key, iv, data)`；
  type 通常写在 `var codex = 0xNN;` 再 `cdy(codex, ...)`；
  `type < 16` → oCode(1-9)，`type >= 16` → nCode（尚未移植）

## 前端与后端的同步点

前端的注释清理掉了，但下面这些"两边必须一致"的约束依然成立：

- **内置网页界面（`app/portal/assets/js/main.js`）里的常量要和后端常量对齐**：
  `DEFAULT_CONN_TIMEOUT` / `DEFAULT_OP_TIMEOUT` / `DEFAULT_WEB_PORT` / `MAX_TIME_WINDOWS`
  对齐 `include/states/States.h`；默认日志目录对齐 `include/utils/Logger.h` 的 `DEFAULT_LOG_DIR`（`"./"` 表示
  程序所在目录）；日志目录长度按后端 `PATH_MAX` 校验，超了会退回默认目录。
- **认证通道取值统一用 `windows/linux/android/ios/macos`**：前端下拉框、`index.html`、
  后端 `parse_channel_json` 三处一致（后端默认值是 Android）。
- **LuCI 页面（`rootfs/www` 与 `rootfs-legacy`）**：默认配置与后端 `s_default_cfg` 是同一套字段；
  `web_port` / `web_external_acc` 是桌面端参数，OpenWrt 版不带网页服务，界面上不提供但字段保留
  ——配置格式两个平台共用，复位时写出去的也必须是完整一套；日志目录规则与 `LoggerDir.c` 的
  `resolve_log_dir` / `get_log_dir` 一致（配置里的 `log_dir` 是**基目录**，日志在它下面的
  `logs` 里；没写或写成 `.` / `./` 时用 `/var/log/esurfing`；相对路径也按它解析，OpenWrt 上
  程序装在只读的 `/usr/bin` 里，不按程序目录解析）。
- **`app/portal/index.html` 里有两处是给打包脚本用的标记**，改动时要一起看
  `scripts/build-portal.sh`：
  - daisyUI 主题那一行**必须独占一行**，打包时整行删除（脚本按行匹配删）；
  - `class-keeper` 那段是让开发模式的 Tailwind 浏览器版生成运行时才用到的类，打包后由
    `input.css` 负责。
- 内置网页界面与 LuCI 界面的**状态指示灯颜色都交给 Alpine 的 `:class` 绑定**，不要直接操作
  `classList`，两者会互相覆盖。
- `main.js` 对 `/api/logs`、`/api/status/sys`、`/api/restartAuth` 这三个较新接口做了降级处理：
  旧后端返回 404 时功能自动降级并提示，不报错崩溃。

## 仓库与打包约定

- **包目录自包含**：`esurfingclient/`、`luci-app-esurfingclient/` 会被整个拷进 OpenWrt SDK
  （`cp -r <包> openwrt-sdk/package/`），因此任何 `../` 形式的引用（`../LICENSE`、
  跨包读版本号）在 SDK 里都会指到别处，而本地看着一切正常。
- **`files/etc/init.d/esurfingclient` 必须是 LF**：`core.autocrlf=true` 时这个文件在 Windows 上
  检出会变成 CRLF，而带 `\r` 的 `#!/bin/sh` 在路由器上跑不起来（报 "not found" —— 解释器
  路径后面跟着一个 `\r`）。它平时在 Windows 工作区里是 LF，很难注意到，直到某次重新检出。
  `.gitattributes` 里按角色钉了 `**/etc/init.d/*`、`**/*.init`、`*.sh`。
- **版本号唯一源是 `esurfingclient/app/CMakeLists.txt` 的 `set(PROGRAM_VERSION_*)` 四行**，
  改完跑 `scripts/sync-version.sh` 分发到两个包的 Makefile 与两处 LuCI 页面（那些是生成物）。
- **安装/升级时谁负责重启服务，opkg 与 apk 不一样**（`files/etc/init.d/esurfingclient`）：
  - **opkg（24.10 及更早）**：升级时 base-files 的 `default_prerm` 会停旧实例、`default_postinst`
    会起新实例，一次安装/升级正好一次重启。所以**包脚本里不能再停/起服务**，实测加了那次
    `restart` 会让 init 脚本打印**两次**"已启动 N 个认证进程实例"，外加一次注定失败的
    `ubus call service delete ... (Not found)`。
  - **apk（25.12 起）**：升级只跑 `post-upgrade` 钩子，**不会先停服务**；而 procd 只在实例定义
    （命令行/参数）变了的时候才重启实例 —— 换个二进制它是不管的。于是只把包脚本里那次重启
    删掉的话，升级完老实例还挂在已经删掉的旧程序上跑（`/proc/<pid>/exe` 会显示 `.… (deleted)`），
    新版本等于没生效，直到有人手动重启。
    所以由 init 脚本自己认这件事：`running_old_binary` 检查有没有实例的 exe 带 "(deleted)"
    标记，有就先停一次（归档日志、等实例登出）再起新的，整个升级仍然只有一次重启。
    opkg 那边 `prerm` 已经把实例停干净，这个检查检测不到东西，是空操作。
  - **apk 的 `pre-deinstall` / `post-deinstall` 只在真卸载时跑**（升级不跑），所以升级不会
    执行 `postrm` 里那段清理，`/etc/config/esurfingclient` 与自启软链接都原样保留
    （`PKG_UPGRADE` 那道保险只需要对付 opkg）。
- **`luci-app` 的 `/tmp/luci-staging` 中转是故意的**：postinst 在目标机上探测属于哪一代 LuCI，
  再决定装 JS 那套还是 Lua/HTM 那套，别"顺手清理"。
- **portal 的网页资源**：`app/portal` 里是源（`index.html` 引用 CDN、`input.css` 是 Tailwind
  输入），发布用的 `assets/css/tailwind.css`、`assets/js/alpine.js`、`daisyui.mjs` 由
  `scripts/build-portal.sh` 生成，不参与版本管理。
