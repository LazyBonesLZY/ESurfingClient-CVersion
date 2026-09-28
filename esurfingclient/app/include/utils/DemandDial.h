#ifndef ESURFINGCLIENT_DEMANDDIAL_H
#define ESURFINGCLIENT_DEMANDDIAL_H

#include <stdbool.h>

/**
 * 按需多拨只在 OpenWrt 的认证进程之间生效 (init 脚本每个账号起一个进程)。
 * 排在配置前面的账号优先; 下载量不够时后面的账号待机, 不发认证流量。
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
