/*
 * 智能多播判定逻辑的离线验证 (不参与构建, 需要时手动编译)
 *
 * 做法: 把 DemandDial.c 直接包含进来, 这样 static 的判定函数与状态都能被测试驱动。
 * 只桩掉日志 / 时钟 / 字符串 / 全局配置, 其余 (文件、getifaddrs、nft 规则生成) 都是真的。
 *
 * 运行 (在 esurfingclient/app/test 下):
 *   gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
 *       -DDEMAND_DIR='"/tmp/esurfingclient-test"' \
 *       -I ../include -I ../src -I ../third_party \
 *       test_demand_dial.c ../third_party/cJSON/cJSON.c -o test_demand_dial
 *   ./test_demand_dial
 *
 * 注意: 推进时钟之后必须同时刷新"对端状态文件"和"采样窗口", 否则对端会被判成已经挂了
 * (状态文件超过 120 秒没更新), 采样窗口也会过期 (超过 30 秒算 0), 那是测试脚手架的问题。
 */
#define _GNU_SOURCE

#include "../src/utils/DemandDial.c"

#include <stdarg.h>

/* ---------------- 桩 ---------------- */

static const char* get_level_name_stub(LogLevel lv);

static uint64_t g_test_now = 1000000;

uint64_t get_cur_tm_ms(void)
{
    return g_test_now;
}

const char* safe_str(const char* str)
{
    return str ? str : "";
}

void log_out(LogLevel level, const char* file, uint32_t line, const char* fmt, ...)
{
    if (level > LOG_LEVEL_INFO) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", get_level_name_stub(level));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static const char* get_level_name_stub(LogLevel lv)
{
    switch (lv)
    {
    case LOG_LEVEL_FATAL: return "FATAL";
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_WARN: return "WARN";
    case LOG_LEVEL_INFO: return "INFO";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    default: return "VERBOSE";
    }
}

/* ---------------- 全局配置 ---------------- */

prog_role_t g_prog_role = ROLE_AUTH;
uint8_t g_prog_account = 2;
uint16_t g_control_port = 8890;
uint16_t g_web_port = 8888;
bool g_web_external_acc = false;
char g_control_token[CONTROL_TOKEN_LEN] = {0};
int g_main_argc = 0;
char** g_main_argv = NULL;
volatile sig_atomic_t g_stop_requested = 0;
int8_t g_prog_cnt = 2;
_Thread_local int8_t tl_thread_idx = 0;
_Thread_local const char* tl_thread_name = "test";
prog_status_t* g_prog_status = NULL;
char g_school_network_symbol[SCHOOL_NETWORK_SYMBOL] = {0};
bool g_thread_keep_alive = false;
bool g_is_webserver_running = false;
bool g_need_exit = false;
bool g_prog_enabled = true;
bool g_need_restart = false;
bool g_cfg_loaded = true;
long g_conn_timeout = 7;
long g_op_timeout = 10;
uint64_t g_start_run_tm = 0;

bool g_demand_dial = true;
uint32_t g_demand_mbps = 100;
uint32_t g_demand_idle_mins = 5;
char g_demand_iface[DEMAND_IFACE_LEN] = {0};
bool g_smart_dial = true;
uint32_t g_demand_up_mbps = 20;
uint32_t g_smart_conns = 512;
uint32_t g_smart_devices = 12;
uint32_t g_smart_hold_secs = 60;
uint8_t g_account_order[ACCOUNT_ORDER_MAX] = {0};
uint32_t g_account_mark[ACCOUNT_ORDER_MAX] = {0};
uint8_t g_account_order_cnt = 0;

/* ---------------- 测试脚手架 ---------------- */

static int s_pass = 0;
static int s_fail = 0;

static void write_peer_state(const uint8_t idx, const int state)
{
    mkdir(DEMAND_DIR, 0755);
    char path[160];
    snprintf(path, sizeof(path), DEMAND_DIR "/account%" PRIu8 ".state", idx);
    FILE* fp = fopen(path, "w");
    if (fp == NULL)
    {
        fprintf(stderr, "无法写 %s\n", path);
        exit(2);
    }
    fprintf(fp, "%d %d %llu %llu\n", (int)getpid(), state, (unsigned long long)g_test_now, (unsigned long long)g_test_now);
    fclose(fp);
}

static void remove_peer_state(const uint8_t idx)
{
    char path[160];
    snprintf(path, sizeof(path), DEMAND_DIR "/account%" PRIu8 ".state", idx);
    unlink(path);
}

/** 铺一段采样窗口: 两次采样, 相隔 10 秒 */
static void set_rates(const uint64_t down_mbps, const uint64_t up_mbps)
{
    const uint64_t base = 1000000000ULL;
    s_samples[0].ms = g_test_now - 10000;
    s_samples[0].rx = base;
    s_samples[0].tx = base;
    s_samples[1].ms = g_test_now;
    s_samples[1].rx = base + down_mbps * 1000000ULL / 8ULL * 10ULL;
    s_samples[1].tx = base + up_mbps * 1000000ULL / 8ULL * 10ULL;
    s_count = 2;
    s_sample_ms = g_test_now;
}

/** 推进时钟, 并把"对端还活着 + 采样窗口是新的"这两件事一起补上 */
static void advance(const uint64_t ms, const uint64_t down_mbps, const uint64_t up_mbps)
{
    g_test_now += ms;
    write_peer_state(1, DEMAND_ONLINE);
    write_peer_state(2, DEMAND_ONLINE);
    set_rates(down_mbps, up_mbps);
}

static const char* act_name(const demand_act_t act)
{
    switch (act)
    {
    case ACT_GO: return "GO(扩容/继续)";
    case ACT_STOP: return "STOP(收回)";
    default: return "HOLD(保持)";
    }
}

static void check(const char* name, const demand_act_t got, const demand_act_t want)
{
    if (got == want)
    {
        s_pass++;
        printf("  [通过] %-58s => %s\n", name, act_name(got));
    }
    else
    {
        s_fail++;
        printf("  [失败] %-58s => %s (期望 %s)\n", name, act_name(got), act_name(want));
    }
}

static void check_bool(const char* name, const bool got, const bool want)
{
    if (got == want)
    {
        s_pass++;
        printf("  [通过] %-58s => %s\n", name, got ? "是" : "否");
    }
    else
    {
        s_fail++;
        printf("  [失败] %-58s => %s (期望 %s)\n", name, got ? "是" : "否", want ? "是" : "否");
    }
}

static void reset_decision_state(void)
{
    s_low_since = 0;
    s_auth_low_since = 0;
    s_last_change_ms = 0;
    s_count = 0;
    s_conns = -1;
    s_devs = -1;
}

int main(void)
{
    /* 两个账号: 配置 1 (排位 0, mark 0x100) 在线, 本进程是配置 2 (排位 1, mark 0x200) */
    g_account_order_cnt = 2;
    g_account_order[0] = 1;
    g_account_order[1] = 2;
    g_account_mark[0] = 0x100;
    g_account_mark[1] = 0x200;

    /* 本进程已经跑了一会儿 (否则"对端状态缺失"会被当成对方正在启动, 见 classify) */
    g_start_run_tm = g_test_now - 200000;

    printf("== 场景 A: 前面没有在线账号, 本账号该直接认证 ==\n");
    remove_peer_state(1);
    reset_decision_state();
    check("ahead=0/blocking=0 => GO", decide(false, false), ACT_GO);

    printf("\n== 场景 B: 下行到阈值, 待机账号该扩容 (智能多播关) ==\n");
    g_smart_dial = false;
    write_peer_state(1, DEMAND_ONLINE);
    reset_decision_state();
    set_rates(120, 0);
    check("下行 120M >= 半条线 (50M) => GO", decide(false, false), ACT_GO);
    reset_decision_state();
    set_rates(40, 0);
    check("下行 40M < 半条线 (50M) => HOLD", decide(false, false), ACT_HOLD);

    printf("\n== 场景 C: 扩容后的静默期 ==\n");
    reset_decision_state();
    set_rates(120, 0);
    check("第一次达到阈值 => GO", decide(false, false), ACT_GO);
    check("静默期内不下发第二次扩容 => HOLD", decide(false, false), ACT_HOLD);
    advance(61000, 120, 0);
    check("静默期过后可以再扩容 => GO", decide(false, false), ACT_GO);

    printf("\n== 场景 D: 空闲收回 ==\n");
    g_smart_dial = true;
    reset_decision_state();
    set_rates(30, 0);
    check("刚掉到收回线以下 => HOLD(要连续)", decide(true, false), ACT_HOLD);
    advance(5 * 60 * 1000, 30, 0);
    check("连续 5 分钟低于一半 => STOP", decide(true, false), ACT_STOP);

    printf("\n== 场景 E: 上行撑着, 不收回 ==\n");
    reset_decision_state();
    set_rates(30, 15);
    check("下行低但上行 15M >= 10M(一半) => HOLD", decide(true, false), ACT_HOLD);
    advance(10 * 60 * 1000, 30, 15);
    check("再等 10 分钟仍不收回 => HOLD", decide(true, false), ACT_HOLD);

    printf("\n== 场景 F: 连接数/设备数只在有流量时才算数 ==\n");
    reset_decision_state();
    set_rates(10, 0);       /* 空载: 合计 10M < 25M 的活动门槛 */
    s_conns = 900;
    s_devs = 40;
    check("空载时连接数/设备数不参与, 不会扩容 => HOLD", decide(false, false), ACT_HOLD);
    check("空载时在线账号开始计收回时间 => HOLD", decide(true, false), ACT_HOLD);
    advance(10 * 60 * 1000, 10, 0);
    s_conns = 900;
    s_devs = 40;
    check("空载也不会因为它们而吊住线路 => STOP", decide(true, false), ACT_STOP);

    printf("\n== 场景 G: 有流量时连接数触发扩容 ==\n");
    reset_decision_state();
    set_rates(30, 5);       /* 合计 35M >= 25M 活动门槛, 但都够不到半条线 */
    s_conns = 900;          /* >= 1 x 512 */
    s_devs = 3;
    check("连接数 900 >= 512 且有流量 => GO", decide(false, false), ACT_GO);

    printf("\n== 场景 H: 有流量时设备数触发扩容 ==\n");
    reset_decision_state();
    set_rates(30, 5);
    s_conns = 10;
    s_devs = 20;            /* >= 1 x 12 */
    check("设备数 20 >= 12 且有流量 => GO", decide(false, false), ACT_GO);

    printf("\n== 场景 I: 上行触发扩容 ==\n");
    reset_decision_state();
    set_rates(5, 25);       /* 上行 25M >= 20M */
    check("上行 25M >= 20M => GO", decide(false, false), ACT_GO);

    printf("\n== 场景 J: 已有两条在线, 阈值按倍数抬升 ==\n");
    g_prog_account = 3;     /* 本进程改成排位 2 */
    g_account_order_cnt = 3;
    g_account_order[0] = 1;
    g_account_order[1] = 2;
    g_account_order[2] = 3;
    g_account_mark[2] = 0x300;
    write_peer_state(1, DEMAND_ONLINE);
    write_peer_state(2, DEMAND_ONLINE);
    reset_decision_state();
    set_rates(250, 0);
    check("前面 2 条在线, 下行 250M >= 2 条线的半速线 (100M) => GO", decide(false, false), ACT_GO);
    reset_decision_state();
    set_rates(80, 0);
    check("下行 80M < 2 条线的半速线 (100M) => HOLD", decide(false, false), ACT_HOLD);

    printf("\n== 场景 K: 认证中的账号 ==\n");
    reset_decision_state();
    set_rates(200, 0);
    write_peer_state(2, DEMAND_STANDBY);
    check("有账号挡在路上时放弃这次认证 => STOP", decide(false, true), ACT_STOP);

    printf("\n== 场景 L: 第一条在线线路永不收回 ==\n");
    g_prog_account = 1;
    g_account_order_cnt = 2;
    g_account_order[0] = 1;
    g_account_order[1] = 2;
    remove_peer_state(2);
    reset_decision_state();
    set_rates(0, 0);
    check("ahead=0 且没人挡路 => GO(不收回)", decide(true, false), ACT_GO);

    printf("\n== 场景 M: 重设备状态文件的读写 ==\n");
    s_online_now = 2;
    g_smart_dial = true;
    g_demand_dial = true;
    heavy_publish("192.168.1.66", g_test_now);
    const char* heavy = heavy_device();
    if (heavy != NULL && strcmp(heavy, "192.168.1.66") == 0)
    {
        s_pass++;
        printf("  [通过] %-58s => %s\n", "发布后能读回重设备", heavy);
    }
    else
    {
        s_fail++;
        printf("  [失败] 重设备读回: %s\n", heavy ? heavy : "(空)");
    }

    heavy_publish("-", g_test_now);
    if (heavy_device() == NULL)
    {
        s_pass++;
        printf("  [通过] %-58s\n", "发布 \"-\" 之后视为没有重设备");
    }
    else
    {
        s_fail++;
        printf("  [失败] 发布 \"-\" 之后仍读到重设备\n");
    }

    s_online_now = 1;
    heavy_publish("192.168.1.66", g_test_now);
    if (heavy_device() == NULL)
    {
        s_pass++;
        printf("  [通过] %-58s\n", "只有一条线路在线时不做设备级独占");
    }
    else
    {
        s_fail++;
        printf("  [失败] 单线路时仍读到重设备\n");
    }

    printf("\n== 场景 N: 过期 / 进程不在的重设备状态会被忽略 ==\n");
    s_online_now = 2;
    heavy_publish("192.168.1.66", g_test_now - 10 * 60 * 1000);
    if (heavy_device() == NULL)
    {
        s_pass++;
        printf("  [通过] %-58s\n", "过期的重设备状态被忽略");
    }
    else
    {
        s_fail++;
        printf("  [失败] 过期状态仍被采用\n");
    }

    printf("\n== 场景 P: 规则写入者判定 (只由最前的活着的进程改规则表) ==\n");
    g_account_order_cnt = 2;
    g_account_order[0] = 1;
    g_account_order[1] = 2;
    g_prog_account = 2;
    g_start_run_tm = g_test_now - 200000;
    remove_peer_state(1);
    check_bool("前面的账号不在了 => 本进程是写入者", steer_is_leader(), true);
    write_peer_state(1, DEMAND_ONLINE);
    check_bool("前面的账号在线 => 本进程不写规则", steer_is_leader(), false);
    g_prog_account = 1;
    check_bool("排在最前的进程自己就是写入者", steer_is_leader(), true);

    printf("\n== 场景 O: 生成转发规则文件 (供 nft 语法校验) ==\n");
    g_smart_dial = true;
    g_demand_dial = true;
    uint32_t from[2] = {0x200, 0x300};
    const bool ok = steer_apply_nft(0x100, from, 2, 0x3f00, "192.168.1.66");
    char src[160];
    snprintf(src, sizeof(src), DEMAND_DIR "/steer.nft");
    FILE* in = fopen(src, "r");
    if (in != NULL)
    {
        FILE* out = fopen("steer_generated.nft", "w");
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
        fclose(in);
        s_pass++;
        printf("  [通过] %-58s (nft 执行结果: %s)\n", "规则文件已生成", ok ? "成功" : "失败(本机无权限, 不影响语法校验)");
    }
    else
    {
        s_fail++;
        printf("  [失败] 规则文件没有生成\n");
    }

    remove_peer_state(1);
    remove_peer_state(2);
    demand_clear();

    printf("\n结果: 通过 %d 项, 失败 %d 项\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
