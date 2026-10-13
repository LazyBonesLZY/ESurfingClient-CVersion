#ifndef ESURFINGCLIENT_DEMANDDIAL_H
#define ESURFINGCLIENT_DEMANDDIAL_H

#include <stdbool.h>

/**
 * 智能多播 (smart_dial): 一个开关, 只在 OpenWrt 的认证进程之间生效
 * (init 脚本每个账号起一个进程)。排在配置前面的账号优先。
 *
 * 打开之后:
 * - 空闲时只认证排在最前的一个账号, 后面的账号待机, 不占校园网在线名额
 * - 有需求时逐条把后面的账号认证上来, 依据是 下行 / 上行 / 连接数 / 在线设备数
 *   四项里任意一项到线 (后三项配置成 0 表示不看)
 * - 连接数、设备数只在"确实有流量"时才算数, 否则一堆设备挂着不动也会被当成该扩容
 * - 空闲 demand_idle_mins 分钟就按"尾线优先"逐条收回, 不会同时退多条
 * - 扩容与收回都不清连接跟踪, 已有连接不被主动打断
 * - 只有下行最猛的那台设备独占多路 (mwan3 均衡), 其它设备一律压回第一条在线线路
 * - 每台设备的流量用 nft 计数集统计 (需要 nft; 只有 iptables 的系统自动退化成线路级改写)
 *
 * 关掉时这些逻辑全部不参与, 行为与没有这个功能时一致。
 */

typedef enum
{
    DEMAND_OFF = 0,
    DEMAND_STANDBY = 1,
    DEMAND_AUTH = 2,
    DEMAND_ONLINE = 3
} demand_state_t;

/** 配置开了、而且至少有两个可用账号、当前是认证进程 */
bool demand_enabled(void);

/** 启动时打一行, 说明本账号的排位和阈值 */
void demand_banner(void);

/**
 * 把本进程的状态写给其它账号看
 * 状态没变时只刷新时间戳, 不重置"进入这个状态"的时刻
 */
void demand_publish(demand_state_t state);

/**
 * 待机中的账号现在该不该开始认证
 * 会顺带采一次流量
 */
bool demand_admit(void);

/**
 * 正在认证或已经在线的账号该不该退回待机
 * @param online 已经认证成功
 */
bool demand_yield(bool online);

/**
 * 长时间睡眠里每秒调用一次: 刷新状态文件, 并在该退回待机时置 is_need_reauth
 * 看门狗线程调用时什么都不做
 */
void demand_touch(void);

/** 进程退出时删掉自己的状态文件 */
void demand_clear(void);

#endif
