# OpenWRT 进阶 - 多拨教程

> [!NOTE]
> 教程版本: v2.0.2-r3

> [!WARNING]
> 本教程为 v2 版本专属, v1 版本不支持该操作
> 
> 教程中的 OpenWRT 版本为 24.10.5, 旧版本其中一些步骤会有些许不同, 需自行查找资料对比修改
> 
> 教程中使用的是双网线双 WAN 多播, 如需单线多播可能需要寻找其它对应资料实现
> 
> 通过 OpenWRT 插件 mwan3 完成
>
> 需要自行提前为 OpenWRT 安装好 mwan3 插件

### 1. 检查 mwan3 插件是否安装

<img alt="Please refresh" width="75%" src="assets/mwan3/01.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/02.png"/>

### 2. 按照图示删除原来的 wan 口, 点保存并应用

<img alt="Please refresh" width="75%" src="assets/mwan3/03.png"/>

### 3. 切换到设备栏, 修改 br-lan 配置

<img alt="Please refresh" width="75%" src="assets/mwan3/04.png"/>

### 4. 取消最后一个 lan 口, 随后保存并应用

> [!NOTE]
> 取消最后一个 lan 口的原因是, 我路由器的 lan4 口靠近 wan 口
> 
> 实际取消哪个都可以, 只需保证后面匹配上即可

<img alt="Please refresh" width="75%" src="assets/mwan3/05.png"/>

### 5. 回到接口栏, 点击 `添加新接口`

<img alt="Please refresh" width="75%" src="assets/mwan3/06.png"/>

### 6. 创建一个 wan1 口, 协议 DHCP 客户端, 设备选择 wan 口, 然后点击创建接口

<img alt="Please refresh" width="75%" src="assets/mwan3/07.png"/>

### 7. 重要的一点, 选择高级设置栏, 将下面的网关跃点设为 10

<img alt="Please refresh" width="75%" src="assets/mwan3/08.png"/>

### 8. 选择防火墙设置栏, 分配 wan 区域防火墙, 然后点击保存

<img alt="Please refresh" width="75%" src="assets/mwan3/09.png"/>

### 9. 创建 wan2 口, 与 wan1 差不多, 不过设备要选择刚刚从 br-lan 那取消的那个 lan 口, 网关跃点设置为 20

<img alt="Please refresh" width="75%" src="assets/mwan3/10.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/11.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/12.png"/>

### 10. 确定无误后点击保存并应用, 插入网线观察是否获取到正确的校园网 IP 地址 (我的是 172.19 开头的)

<img alt="Please refresh" width="75%" src="assets/mwan3/13.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/14.png"/>

### 11. 接下来是 mwan3 操作, 按照图示切换到 mwan 管理器的接口栏, 并删除所有默认接口

<img alt="Please refresh" width="75%" src="assets/mwan3/15.png"/>

### 12. 随后添加 wan1 接口, 按照图示进行配置, wan2 接口同理

<img alt="Please refresh" width="75%" src="assets/mwan3/16.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/17.png"/>

### 13. 确定无误后点击保存并应用

<img alt="Please refresh" width="75%" src="assets/mwan3/18.png"/>

### 14. 切换到成员栏, 并删除所有默认成员

<img alt="Please refresh" width="75%" src="assets/mwan3/19.png"/>

### 15. 随后添加 wan1_m1 成员, 按照图示进行配置, wan2_m2 成员同理

<img alt="Please refresh" width="75%" src="assets/mwan3/20.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/21.png"/>

### 16. 确定无误后点击保存并应用

<img alt="Please refresh" width="75%" src="assets/mwan3/22.png"/>

### 17. 切换到策略栏, 并删除除 balanced 策略以外的所有默认策略

<img alt="Please refresh" width="75%" src="assets/mwan3/23.png"/>

### 18. 点击编辑 balanced 策略, 按照图示进行配置

<img alt="Please refresh" width="75%" src="assets/mwan3/24.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/25.png"/>

### 19. 确定无误后点击保存并应用

<img alt="Please refresh" width="75%" src="assets/mwan3/26.png"/>

### 20. 切换到规则栏, 并删除 default_rule_v4 以外的所有默认规则

<img alt="Please refresh" width="75%" src="assets/mwan3/27.png"/>

### 21. 点击编辑 default_rule_v4 规则, 按用途设置粘滞模式后保存

**智能多播 / 单设备多连接叠加需关闭粘滞 (`sticky=0`)**。粘滞按源 IP 记住 WAN, 即使认证多条线路, 同一设备也可能仍然全走同一条。使用相同 metric 的均衡成员, 按连接分流而非逐包轮询。确实需要同出口的应用可配置优先级更高的单独规则, 不要把下载设备匹配到粘滞规则。

<img alt="Please refresh" width="75%" src="assets/mwan3/28.png"/>

<img alt="Please refresh" width="75%" src="assets/mwan3/29.png"/>

### 22. 确定无误后点击保存并应用

<img alt="Please refresh" width="75%" src="assets/mwan3/30.png"/>

### 23. 按照图示观察, wan1 和 wan2 状态为离线即为配置好 mwan3 了

<img alt="Please refresh" width="75%" src="assets/mwan3/31.png"/>

### 24. 安装认证程序 - [下载传送门](https://github.com/BadGhost520/ESurfingClient-CVersion/releases/latest)

### 25. 退出并重新登录后台, 打开 `服务` >> `ESurfing 客户端`

### 26. 填写好认证配置 (`标记值` 的用途 > [传送门](#v202-r3-版本新增-标记值))

<img alt="Please refresh" width="75%" src="assets/mwan3/32.png"/>

### 27. 运行程序前的检查, 输入 ip rule show 检查是否有如图红框的规则, 有则正常, 没有则检查前面是否有步骤做错了

<img alt="Please refresh" width="75%" src="assets/mwan3/33.png"/>

### 28. 输入 /etc/init.d/esurfingclient reload 重载 init.d 配置文件, 输入 tail -f -n 50 /var/log/esurfing/logs/run.log 查看运行情况

<img alt="Please refresh" width="75%" src="assets/mwan3/34.png"/>

> [!NOTE]
> 如果一切正常且下游设备有网了
> 
> 那么就恭喜你实现了双 WAN 口多播
> 
> 上行和下行在 `多线程` 上传和下载时都会受到叠加 buff

## v2.0.2-r3 版本新增 (标记值)

### 这是一个用于给一些特殊网络环境使用的方案

#### 举个例子

#### 假如 wan1 口接的是不用认证就有网的网络, wan2 则是需要认证的校园网络

#### 小明按先 wan1, 后 wan2 的顺序填写了 mwan3 的 `接口` 配置

#### 然后填写了 `仅一个` ESurfing 客户端的认证配置并启动 ESurfing 客户端, 发现不行, 无法认证, 总提示 `已连接到互联网`

#### 因为 wan1 和 wan2 会被 mwan3 按填写顺序分配 0x100 和 0x200 的标记值

#### wan1 只允许 0x100 和没标签的数据包通过, wan2只允许 0x200 和没标签的数据包通过

#### 而 ESurfing 客户端的自动分配标记值会按照可用配置按顺序打标签

#### 即, 可用配置 1 打上 0x100, 可用配置 2 打上 0x200, 以此类推

#### 小明的认证配置无法进行认证, 总提示 `已连接到互联网` 的原因就是这个

#### 只填了配置 1, 然后程序自动打上 0x100 的标签, 数据包就跑到 wan1 有网的口去了

### 所以! 隆重推出我们的自定义标记值功能! 👏

<img alt="Please refresh" width="75%" src="assets/mwan3/35.png"/>

### 只需要在配置页给每个认证配置填写上标记值, 对应的配置在发包的时候就会被打上对应的标签

### 然后数据包就会乖乖地按照被打上的标签去走对应的通道

### 妈妈再也不用担心我的数据包乱跑辣!

> [!WARNING]
> 标记值是一个十六进制数, 以 0x 开头, 而且必须是 0x100 的倍数
> 
> 可以直接在终端用 ip rule show 来查看要打什么标签
> 
> 以免数据包标签管理混乱, 只要有一个认证配置填了自定义标记值
> 
> 那么其它的认证配置就都要填, 不然会被程序跳过加载

## 按需多拨

多条 WAN 都认证着的时候, 校园网侧会占多个在线名额。平时没有大下载, 可以在配置里打开 `demand_dial`:

- 账号在配置里的顺序就是优先级, 第一个一直在线
- 最近几秒的采样峰值下行达到 50 Mbps (默认阈值 100 的一半) 就认证第二个; 两条都在线后, 再达到 100 Mbps 才认证第三个。新线路拨上后保留现有连接, 后续新建的多线程连接才有机会分到新线路
- 下行连续 5 分钟低于对应扩容线的一半, 就从最后扩上去的那个账号登出
- 第一个账号连续 2 分钟上不了线, 第二个会顶上; 第一个恢复后, 第二个按上面的空闲规则退下

统计的是 WAN 口接收流量 (有默认路由的网口, 或 `demand_iface` 指定的网口), 不是某一个连接。单线程下载不会因为多开账号变快; 登出一个账号时, 还挂在那条 WAN 上的连接会断。新上来的账号要等 mwan3 把它标成在线, 才会开始分到流量。

## 智能多播

按需多拨只管"要不要多开线路", 智能多播 (`smart_dial`, 需要同时打开 `demand_dial`) 管的是"这些线路怎么分":

- 扩容依据从只看下行扩到四项: 下行 (`demand_mbps`)、上行 (`demand_up_mbps`)、连接数 (`smart_conns`)、在线设备数 (`smart_devices`)。任意一项到线就再认证一个账号, 依旧是一个一个加。下行与上行沿用按需多拨的口径 (最近一秒峰值到 `N × 阈值` 的一半), 连接数与设备数到配置值才算
- 连接数、设备数只在确实有流量时才算数 (总下行 + 总上行 ≥ 下行阈值的 1/4)。不然家里十几台设备一直挂着, 空载也会被当成"设备多, 该扩容", 白占在线名额
- 收回还是老规矩: 四项都低于各自收回线 (阈值的一半) 并持续 `demand_idle_mins` 分钟, 就从最后扩上去的账号开始登出
- **只有下载最猛的那台设备独占多路**: 它的下行达到 `demand_mbps` 之后, 由 mwan3 照常把它分流到多条线路上; 其它设备一律被压回第一条在线线路, 不再跟它抢。重设备要连续两次采样确认; 它的下行掉到阈值一半以下连续三次, 或者线路回到 1 条, 就取消独占
- 每次扩容或收回之后有 `smart_hold_secs` (默认 60 秒) 的静默期, 防止在阈值附近来回抖动

### 启用前检查 (OpenWrt / ImmortalWrt 24.10, 含 Filogic)

1. `mwan3` 均衡策略中参与叠加的成员使用相同 metric, 下载设备匹配的规则关闭 `sticky`。当前程序不自动覆盖你的 mwan3 配置。
2. **关闭软件和硬件流量卸载**, 或由部署方明确排除受管理流量。flowtable/PPE 会绕过本程序的 nft 计数链, 导致重设备识别失真。常规 fw4 配置可先查看, 需要关闭时执行:

   ```sh
   uci show firewall | sed -n '/flow_offloading/p'
   uci set firewall.@defaults[0].flow_offloading='0'
   uci set firewall.@defaults[0].flow_offloading_hw='0'
   uci commit firewall
   /etc/init.d/firewall restart
   ```

   自定义固件的 SFE/NSS/PPE 加速还需在其对应界面检查; 关闭这两个 UCI 项不代表所有额外加速都已关闭。参考 [内核 flowtable 行为](https://docs.kernel.org/networking/nf_flowtable.html)。
3. `demand_iface` 填写 WAN 的 Linux 设备名 (如 `eth1`、`pppoe-wan`), 不是 LuCI 逻辑接口名。用 `ip -4 route show table all` 检查并列全, 防止统计接口变化或漏算。
4. 当前只管理 IPv4 会话; IPv6 不受设备级独占规则控制。单条 TCP 连接不会跨 WAN 聚合, 叠加需要多个新会话。

### 稳定性上的取舍

- 转发规则是**整套一次性下发**的 (一个 nft 事务里 `flush chain` 再 `add rule`), 消除规则空窗; 这不等于实际线路退网、NAT、mwan3 健康切换都无损
- **扩容、收回、设备独占变化均不全表清理连接跟踪**, 不主动迁移已有 NAT 会话。规则只在会话第一次确认前选择出口, 并保存最终连接标记; 后续包沿原路径。退网线路上的既有连接仍可能中断, 当前尚无按 WAN 的 DRAIN/排空功能, 应用需自行重建受影响连接
- 重设备由**排在最前面的那条在线线路**统一统计并写进 `/var/run/esurfingclient/heavy.state`, 其它进程只读。要是各进程各算各的, 采样时刻不同会得出不同的"重设备", 规则就会在两个结果之间来回改
- 每设备统计依赖 nft 的计数集合 (`ip esurf_demand` 表)。**只有 iptables 的系统自动退化成线路级改写**, 不做设备级独占, 扩容与收回照常
- 被压回第一条在线线路的设备, 在那条线路真的断掉时会短暂受影响 (最长到那条线路的认证进程被判死、状态文件过期为止, 通常 5-20 秒); 重设备自己不受影响
- 独占规则只动"局域网 → 外网"的转发 (`ip daddr != @lan_nets`), 内网互访不受影响; 认证进程自己发的包走 OUTPUT, 也不受影响, 所以待机账号仍然能认证
