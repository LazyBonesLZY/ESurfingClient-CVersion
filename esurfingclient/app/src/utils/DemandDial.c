#include "utils/DemandDial.h"

#include "states/States.h"

#include "utils/PlatformUtils.h"
#include "utils/Logger.h"

#include "cJSON/cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * 状态目录。可以被测试覆盖 (app/test/test_demand_dial.c 会指到 /tmp 下),
 * 正常构建时用默认值。
 */
#ifndef DEMAND_DIR
#define DEMAND_DIR "/var/run/esurfingclient"
#endif

#define DEMAND_SAMPLES 10

#define DEMAND_FRESH_MS 15000

#define DEMAND_STUCK_MS 120000

#define DEMAND_AUTH_ABORT_MS 30000

#define DEMAND_IFACE_MAX 8

#define DEMAND_IFACE_NAME 16

/**
 * 智能多播用到的常量
 *
 * 每设备流量靠 nft 的动态计数集统计: 集合元素有 10 分钟寿命, 不活跃的设备会自己老化掉,
 * 不会无限堆积; 元素数量上限固定, 免得一台机器上几百个设备把内存吃光。
 */
#define SMART_ACCT_SET_LAN "lan_nets"

#define SMART_ACCT_SET_DL "dev_dl"

#define SMART_ACCT_SET_UL "dev_ul"

#define SMART_ACCT_CHAIN "acct"

#define SMART_ACCT_MAX 64

#define SMART_ACCT_SAMPLE_MS 5000

#define SMART_HEAVY_STREAK 2

#define SMART_HEAVY_IDLE_STREAK 3

#define SMART_MAX_LAN_NETS 8

#define SMART_LAN_CIDR_LEN 24

#define SMART_CMD_BUF 32768

typedef struct
{
    uint64_t ms;
    uint64_t rx;
    uint64_t tx;
} demand_sample_t;

/** 一台设备的计数 (bytes 是 nft 里的累计值, bps 是与上次采样比出来的速率) */
typedef struct
{
    char ip[IP_LEN];
    uint64_t bytes;
    uint64_t bps;
} dev_rate_t;

static int s_state = -1;
static uint64_t s_since = 0;
static uint64_t s_touch_ms = 0;
static uint64_t s_low_since = 0;
static uint64_t s_auth_low_since = 0;
static uint64_t s_last_change_ms = 0;

static demand_sample_t s_samples[DEMAND_SAMPLES];
static int s_count = 0;
static uint64_t s_sample_ms = 0;

static int s_dbg_ahead = 0;
static int s_dbg_blocking = 0;
static uint64_t s_dbg_bps = 0;
static uint64_t s_dbg_up_bps = 0;
static int s_dbg_conns = -1;
static int s_dbg_devs = -1;
static bool s_dbg_high = false;
static bool s_dbg_low = false;

static int s_conns = -1;
static int s_devs = -1;

static int s_last_admit = -1;
static bool s_yield_logged = false;
static bool s_rank_warned = false;
static bool s_rx_warned = false;
static bool s_dir_warned = false;
static char s_iface_logged[DEMAND_IFACE_LEN] = {0};

/* ---- 智能多播: 每设备流量与"重设备" ---- */

static dev_rate_t s_dev_prev[SMART_ACCT_MAX];
static int s_dev_prev_n = 0;
static uint64_t s_dev_sample_ms = 0;

static char s_heavy[IP_LEN] = {0};
static char s_heavy_cand[IP_LEN] = {0};
static int s_heavy_cand_n = 0;
static int s_heavy_idle_n = 0;
static int s_online_now = 0;

static bool s_acct_warned = false;
static bool s_lan_warned = false;

/**
 * 规则被外力抹掉时的自愈标记
 *
 * 规则表可能被别的进程或人手删掉 (重启某个账号实例时 init 脚本就会删一次),
 * 而"已下发"的缓存还留在这里 —— 那样规则就永远不会再补上。每设备计数读不到时
 * 认为表可能没了, 下一次强制重下一遍。
 */
static bool s_steer_dirty = false;

static void demand_steer(const bool force);

static bool heavy_path(char* out, const size_t len, const bool tmp);

typedef enum
{
    ACT_HOLD = 0,
    ACT_GO = 1,
    ACT_STOP = 2
} demand_act_t;

bool demand_enabled(void)
{
    return g_demand_dial && g_account_order_cnt > 1 && g_prog_role == ROLE_AUTH && g_prog_account != 0;
}

static int my_index(void)
{
    for (uint8_t i = 0; i < g_account_order_cnt; i++)
    {
        if (g_account_order[i] == g_prog_account) return (int)i;
    }
    return -1;
}

static bool state_path(char* out, const size_t len, const uint8_t idx, const bool tmp)
{
    int n;
    if (tmp)
    {
        n = snprintf(out, len, DEMAND_DIR "/account%" PRIu8 ".state.tmp.%d", idx, (int)getpid());
    }
    else
    {
        n = snprintf(out, len, DEMAND_DIR "/account%" PRIu8 ".state", idx);
    }
    return n > 0 && (size_t)n < len;
}

static void ensure_dir(void)
{
    if (mkdir(DEMAND_DIR, 0755) == 0 || errno == EEXIST) return;
    if (s_dir_warned) return;

    LOG_WARN("无法创建 %s: %s, 按需多拨无法在账号之间协同", DEMAND_DIR, strerror(errno));
    s_dir_warned = true;
}

static void write_file(const uint64_t now)
{
    if (g_prog_account == 0 || s_state < 0) return;

    ensure_dir();

    char path[160];
    char tmp[160];
    if (state_path(path, sizeof(path), g_prog_account, false) == false) return;
    if (state_path(tmp, sizeof(tmp), g_prog_account, true) == false) return;

    FILE* fp = fopen(tmp, "w");
    if (fp == NULL)
    {
        if (s_dir_warned == false)
        {
            LOG_WARN("无法写入按需多拨状态 %s: %s", tmp, strerror(errno));
            s_dir_warned = true;
        }
        return;
    }

    fprintf(fp, "%d %d %" PRIu64 " %" PRIu64 "\n", (int)getpid(), s_state, s_since, now);
    if (fclose(fp) != 0)
    {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0)
    {
        unlink(tmp);
    }
}

void demand_publish(const demand_state_t state)
{
    if (demand_enabled() == false) return;

    const uint64_t now = get_cur_tm_ms();
    if ((int)state != s_state)
    {
        s_state = (int)state;
        s_since = now;
        s_low_since = 0;
        s_auth_low_since = 0;
        s_yield_logged = false;
    }
    write_file(now);
    demand_steer(false);
}

void demand_clear(void)
{
    s_state = -1;
    if (g_prog_account == 0) return;

    char path[160];
    if (state_path(path, sizeof(path), g_prog_account, false) == false) return;
    unlink(path);

    /* 自己写的那份"重设备"状态也一并收掉, 免得下次启动读到上一条命留下的 pid */
    char heavy[160];
    if (heavy_path(heavy, sizeof(heavy), false) == false) return;

    FILE* fp = fopen(heavy, "r");
    if (fp == NULL) return;

    int pid = 0;
    if (fscanf(fp, "%d", &pid) == 1 && pid == (int)getpid()) unlink(heavy);
    fclose(fp);
}

void demand_banner(void)
{
    demand_steer(false);
    if (g_demand_dial == false) return;

    if (g_account_order_cnt <= 1)
    {
        LOG_INFO("按需多拨已开启, 可用账号不足 2 个, 本账号照常认证");
        return;
    }

    const int idx = my_index();
    const char* iface = g_demand_iface[0] != '\0' ? g_demand_iface : "自动";
    if (idx < 0)
    {
        LOG_WARN("按需多拨已开启, 但找不到本账号的排位, 本账号照常认证");
        return;
    }

    LOG_INFO("按需多拨已开启, 本账号排位 %d/%" PRIu8 ", 下行阈值 %" PRIu32 " Mbps, 连续 %" PRIu32
             " 分钟低于一半则收回, 统计网口: %s",
        idx + 1, g_account_order_cnt, g_demand_mbps, g_demand_idle_mins, iface);

    if (g_smart_dial == false)
    {
        LOG_INFO("智能多播未开启 (smart_dial=false), 只按下行扩容, 不做设备级多路独占");
        return;
    }

    LOG_INFO("智能多播已开启: 上行阈值 %" PRIu32 " Mbps, 连接数阈值 %" PRIu32
             " (0=不看), 设备数阈值 %" PRIu32 " (0=不看), 变更静默期 %" PRIu32 " 秒",
        g_demand_up_mbps, g_smart_conns, g_smart_devices, g_smart_hold_secs);
    LOG_INFO("智能多播: 只有下行最猛 (≥ %" PRIu32 " Mbps) 的那台设备独占多路, 其余设备走第一条在线线路",
        g_demand_mbps);
}

static bool add_iface(char names[][DEMAND_IFACE_NAME], int* count, const char* name)
{
    if (name == NULL || name[0] == '\0' || *count >= DEMAND_IFACE_MAX) return false;
    if (strlen(name) >= DEMAND_IFACE_NAME) return false;
    if (strcmp(name, "lo") == 0) return false;

    for (int i = 0; i < *count; i++)
    {
        if (strcmp(names[i], name) == 0) return false;
    }

    snprintf(names[*count], DEMAND_IFACE_NAME, "%s", name);
    (*count)++;
    return true;
}

static int collect_ifaces(char names[][DEMAND_IFACE_NAME])
{
    int count = 0;

    if (g_demand_iface[0] != '\0')
    {
        char buf[DEMAND_IFACE_LEN];
        snprintf(buf, sizeof(buf), "%s", g_demand_iface);

        char* cursor = buf;
        while (*cursor != '\0' && count < DEMAND_IFACE_MAX)
        {
            while (*cursor == ' ' || *cursor == ',' || *cursor == ';') cursor++;
            if (*cursor == '\0') break;

            char* start = cursor;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != ',' && *cursor != ';') cursor++;
            if (*cursor != '\0')
            {
                *cursor = '\0';
                cursor++;
            }
            add_iface(names, &count, start);
        }
        return count;
    }

    FILE* fp = fopen("/proc/net/route", "r");
    if (fp == NULL) return 0;

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char name[DEMAND_IFACE_NAME];
        unsigned int dest = 1;
        if (sscanf(line, "%15s %x", name, &dest) != 2) continue;
        if (dest != 0) continue;
        add_iface(names, &count, name);
    }
    fclose(fp);
    return count;
}

static void log_ifaces(char names[][DEMAND_IFACE_NAME], const int count)
{
    char joined[DEMAND_IFACE_LEN];
    joined[0] = '\0';
    size_t used = 0;

    for (int i = 0; i < count; i++)
    {
        const int n = snprintf(joined + used, sizeof(joined) - used, "%s%s", i == 0 ? "" : " ", names[i]);
        if (n < 0 || (size_t)n >= sizeof(joined) - used) break;
        used += (size_t)n;
    }

    if (strcmp(joined, s_iface_logged) == 0) return;
    snprintf(s_iface_logged, sizeof(s_iface_logged), "%s", joined);

    if (count == 0)
    {
        LOG_WARN("按需多拨没有可统计的网口 (默认路由或 demand_iface), 上下行按 0 计, 不会扩容");
        return;
    }
    LOG_INFO("按需多拨统计网口: %s", joined);
}

/**
 * @brief 读统计网口的累计收发字节
 *
 * 收 = 下行, 发 = 上行。/proc/net/dev 的格式是 接收 8 项之后紧跟发送 8 项,
 * 所以要连着解析 9 个数才能拿到发送字节数。
 */
static bool read_netdev(uint64_t* rx_out, uint64_t* tx_out)
{
    char names[DEMAND_IFACE_MAX][DEMAND_IFACE_NAME];
    const int count = collect_ifaces(names);
    log_ifaces(names, count);
    if (count == 0)
    {
        *rx_out = 0;
        *tx_out = 0;
        return false;
    }

    FILE* fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) return false;

    uint64_t rx_sum = 0;
    uint64_t tx_sum = 0;
    int matched = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char* colon = strchr(line, ':');
        if (colon == NULL) continue;
        *colon = '\0';

        char* name = line;
        while (*name == ' ') name++;

        bool wanted = false;
        for (int i = 0; i < count; i++)
        {
            if (strcmp(name, names[i]) == 0)
            {
                wanted = true;
                break;
            }
        }
        if (wanted == false) continue;

        unsigned long long field[9] = {0};
        const int n = sscanf(colon + 1, " %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &field[0], &field[1], &field[2], &field[3], &field[4],
            &field[5], &field[6], &field[7], &field[8]);
        if (n != 9) continue;

        rx_sum += (uint64_t)field[0];
        tx_sum += (uint64_t)field[8];
        matched++;
    }
    fclose(fp);

    if (matched == 0) return false;
    *rx_out = rx_sum;
    *tx_out = tx_sum;
    return true;
}

/** 连接跟踪表里的连接数, 读不到返回 -1 */
static int read_conntrack_count(void)
{
    FILE* fp = fopen("/proc/sys/net/netfilter/nf_conntrack_count", "r");
    if (fp == NULL) return -1;

    int value = -1;
    if (fscanf(fp, "%d", &value) != 1) value = -1;
    fclose(fp);
    return value;
}

/**
 * @brief 在线设备数 (局域网里的设备)
 *
 * 优先数 ARP 表 (已经通上话的设备), ARP 表空的时候退回 DHCP 租约文件:
 * 设备刚连上还没产生流量时 ARP 里可能还没有它。
 */
static int read_device_count(void)
{
    int arp_count = 0;
    bool arp_readable = false;
    FILE* fp = fopen("/proc/net/arp", "r");
    if (fp != NULL)
    {
        arp_readable = true;
        char line[256];
        bool header = true;
        while (fgets(line, sizeof(line), fp) != NULL)
        {
            if (header)
            {
                header = false;
                continue;
            }

            char ip[64] = {0};
            char hwtype[64] = {0};
            char hwaddr[64] = {0};
            char mask[64] = {0};
            char dev[64] = {0};
            unsigned int flags = 0;
            if (sscanf(line, "%63s %63s %x %63s %63s %63s", ip, hwtype, &flags, hwaddr, mask, dev) != 6) continue;
            if ((flags & 0x2U) == 0) continue;
            if (strcmp(hwaddr, "00:00:00:00:00:00") == 0) continue;
            arp_count++;
        }
        fclose(fp);
    }

    if (arp_count > 0) return arp_count;

    int lease_count = 0;
    bool leases_readable = false;
    FILE* leases = fopen("/tmp/dhcp.leases", "r");
    if (leases != NULL)
    {
        leases_readable = true;
        char line[256];
        while (fgets(line, sizeof(line), leases) != NULL)
        {
            if (line[0] == '\n' || line[0] == '\0') continue;
            lease_count++;
        }
        fclose(leases);
    }

    if (arp_readable == false && leases_readable == false) return -1;
    return lease_count;
}

/**
 * @brief 收集本机的局域网网段
 *
 * 用来把"局域网 <-> 外网"的转发流量从其它转发流量里摘出来, 并且保证改写标记时
 * 不会碰到内网互访的包。WAN 口 (demand_iface 指定的, 或是有默认路由的那些) 与
 * 点对点接口 (pppoe 之类) 都排除掉。
 */
static int collect_lan_nets(char nets[][SMART_LAN_CIDR_LEN], const int max)
{
    char wan[DEMAND_IFACE_MAX][DEMAND_IFACE_NAME];
    const int wan_n = collect_ifaces(wan);

    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0) return 0;

    int count = 0;
    for (struct ifaddrs* p = ifa; p != NULL && count < max; p = p->ifa_next)
    {
        if (p->ifa_addr == NULL || p->ifa_netmask == NULL) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        if ((p->ifa_flags & IFF_UP) == 0) continue;
        if ((p->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if ((p->ifa_flags & IFF_POINTOPOINT) != 0) continue;

        bool is_wan = false;
        for (int i = 0; i < wan_n; i++)
        {
            if (strcmp(p->ifa_name, wan[i]) == 0)
            {
                is_wan = true;
                break;
            }
        }
        if (is_wan) continue;

        const struct sockaddr_in* addr = (const struct sockaddr_in*)p->ifa_addr;
        const struct sockaddr_in* mask = (const struct sockaddr_in*)p->ifa_netmask;
        const uint32_t a = ntohl(addr->sin_addr.s_addr);
        const uint32_t m = ntohl(mask->sin_addr.s_addr);
        if (m == 0) continue;

        int bits = 0;
        for (uint32_t bit = 0x80000000U; bit != 0 && (m & bit) != 0; bit >>= 1) bits++;

        struct in_addr net_addr;
        net_addr.s_addr = htonl(a & m);
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &net_addr, text, sizeof(text)) == NULL) continue;

        char cidr[SMART_LAN_CIDR_LEN];
        const int n = snprintf(cidr, sizeof(cidr), "%s/%d", text, bits);
        if (n <= 0 || (size_t)n >= sizeof(cidr)) continue;

        bool dup = false;
        for (int i = 0; i < count; i++)
        {
            if (strcmp(nets[i], cidr) == 0)
            {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        snprintf(nets[count], SMART_LAN_CIDR_LEN, "%s", cidr);
        count++;
    }

    freeifaddrs(ifa);
    return count;
}

static int run_exec(char* const argv[])
{
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status) == false) return -1;
    return WEXITSTATUS(status);
}

/**
 * @brief 跑一条命令并把它的输出收下来
 *
 * 一定要读到 EOF 再 waitpid: 缓冲区满了就不读的话, 子进程会卡在 write 上,
 * 父进程又卡在 waitpid 上, 双方都不动。
 */
static int run_capture(char* const argv[], char* out, const size_t out_len)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);

    size_t used = 0;
    char sink[256];
    for (;;)
    {
        const bool keep = used + 1 < out_len;
        char* dst = keep ? (out + used) : sink;
        const size_t cap = keep ? (out_len - used - 1) : sizeof(sink);

        const ssize_t n = read(fds[0], dst, cap);
        if (n > 0)
        {
            if (keep) used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    out[used] = '\0';
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (WIFEXITED(status) == false) return -1;
    return WEXITSTATUS(status);
}

static const char* find_tool(const char* a, const char* b)
{
    if (access(a, X_OK) == 0) return a;
    if (access(b, X_OK) == 0) return b;
    return NULL;
}

static const char* nft_tool(void)
{
    return find_tool("/usr/sbin/nft", "/usr/bin/nft");
}

static uint32_t mwan_mask(void)
{
    static uint32_t mask = 0;
    if (mask != 0) return mask;

    mask = 0x3f00;
    FILE* fp = fopen("/etc/config/mwan3", "r");
    if (fp == NULL) return mask;

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char* pos = strstr(line, "mmx_mask");
        if (pos == NULL) continue;

        pos += strlen("mmx_mask");
        while (*pos == ' ' || *pos == '\'' || *pos == '"' || *pos == '\t') pos++;

        char* end = NULL;
        const unsigned long value = strtoul(pos, &end, 0);
        if (end != pos && value != 0 && value <= 0xffffffffUL) mask = (uint32_t)value;
        break;
    }
    fclose(fp);
    return mask;
}

static void flush_conntrack(void)
{
    FILE* fp = fopen("/proc/net/nf_conntrack", "w");
    if (fp == NULL) return;

    fputc('f', fp);
    fclose(fp);
    LOG_INFO("按需多拨: 已清掉连接跟踪, 连接会按当前在线线路重新建立");
}

static bool pid_alive(const int pid)
{
    if (pid <= 0) return false;
    if (kill((pid_t)pid, 0) == 0) return true;
    return errno == EPERM;
}

static bool read_peer(const uint8_t idx, int* pid, int* state, uint64_t* since, uint64_t* updated)
{
    char path[160];
    if (state_path(path, sizeof(path), idx, false) == false) return false;

    FILE* fp = fopen(path, "r");
    if (fp == NULL) return false;

    const int n = fscanf(fp, "%d %d %" SCNu64 " %" SCNu64, pid, state, since, updated);
    fclose(fp);
    return n == 4;
}

static uint64_t fresh_ms(void)
{
    uint64_t net = (uint64_t)(g_conn_timeout + g_op_timeout) * 1000ULL + 10000ULL;
    if (net < DEMAND_FRESH_MS) return DEMAND_FRESH_MS;
    if (net > DEMAND_STUCK_MS) return DEMAND_STUCK_MS;
    return net;
}

static bool within(const uint64_t now, const uint64_t stamp, const uint64_t window)
{
    if (stamp > now) return true;
    return now - stamp < window;
}

/** 定长拷贝: 设备 IP 最长 IP_LEN-1, 超长直接截断 (来源已判过长度, 这里只是兜底) */
static void copy_ip(char* dst, const size_t len, const char* src)
{
    if (len == 0) return;

    size_t n = strlen(src);
    if (n >= len) n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool account_is_online(const int index)
{
    if (index < 0 || index >= g_account_order_cnt) return false;

    const uint8_t idx = g_account_order[index];
    if (idx == g_prog_account) return s_state == DEMAND_ONLINE;

    int pid = 0;
    int state = -1;
    uint64_t since = 0;
    uint64_t updated = 0;
    if (read_peer(idx, &pid, &state, &since, &updated) == false) return false;

    const uint64_t now = get_cur_tm_ms();
    if (pid_alive(pid) == false || within(now, updated, fresh_ms()) == false) return false;
    return state == DEMAND_ONLINE;
}

/** 当前在线的账号数 (含自己), 供"有没有多路"的判断使用 */
static int online_count(void)
{
    int count = 0;
    for (int i = 0; i < g_account_order_cnt; i++)
    {
        if (account_is_online(i)) count++;
    }
    return count;
}

/** 排在第一条的在线线路的下标, 没有在线线路返回 -1 */
static int primary_online_index(void)
{
    for (int i = 0; i < g_account_order_cnt; i++)
    {
        if (account_is_online(i)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------
 * 智能多播: 重设备的跨进程共享
 *
 * 重设备是"哪台设备下载最猛", 每个进程自己采样的时刻不同, 算出来的结果就可能不同,
 * 而转发规则是一整套换掉的 —— 两个进程各按自己的结果下发, 规则就会来回改。
 * 所以只让排在最前面的那条在线线路负责统计, 结果写进状态文件, 其它进程只读。
 * ------------------------------------------------------------------ */

static bool heavy_path(char* out, const size_t len, const bool tmp)
{
    int n;
    if (tmp)
    {
        n = snprintf(out, len, DEMAND_DIR "/heavy.state.tmp.%d", (int)getpid());
    }
    else
    {
        n = snprintf(out, len, DEMAND_DIR "/heavy.state");
    }
    return n > 0 && (size_t)n < len;
}

static void heavy_publish(const char* ip, const uint64_t now)
{
    ensure_dir();

    char path[160];
    char tmp[160];
    if (heavy_path(path, sizeof(path), false) == false) return;
    if (heavy_path(tmp, sizeof(tmp), true) == false) return;

    FILE* fp = fopen(tmp, "w");
    if (fp == NULL) return;

    fprintf(fp, "%d %s %" PRIu64 "\n", (int)getpid(), (ip != NULL && ip[0] != '\0') ? ip : "-", now);
    if (fclose(fp) != 0)
    {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) unlink(tmp);
}

/** 读共享的重设备 (谁负责统计就由谁写); 没有或已经过期返回 NULL */
static const char* heavy_device(void)
{
    static char out[IP_LEN];

    out[0] = '\0';
    if (g_smart_dial == false || g_demand_dial == false) return NULL;
    if (s_online_now < 2) return NULL;

    char path[160];
    if (heavy_path(path, sizeof(path), false) == false) return NULL;

    FILE* fp = fopen(path, "r");
    if (fp == NULL) return NULL;

    int pid = 0;
    char ip[IP_LEN] = {0};
    uint64_t updated = 0;
    const int n = fscanf(fp, "%d %15s %" SCNu64, &pid, ip, &updated);
    fclose(fp);
    if (n != 3) return NULL;
    if (strcmp(ip, "-") == 0) return NULL;

    const uint64_t now = get_cur_tm_ms();
    if (pid_alive(pid) == false) return NULL;
    if (within(now, updated, fresh_ms()) == false) return NULL;

    copy_ip(out, sizeof(out), ip);
    return out;
}

/* ------------------------------------------------------------------
 * 智能多播: 每设备流量统计
 * ------------------------------------------------------------------ */

/** 解析 nft -j list set 的输出, 取 dev_dl 里每个元素的累计字节数 */
static int acct_parse(const char* json, dev_rate_t* out, const int max)
{
    cJSON* root = cJSON_Parse(json);
    if (root == NULL) return -1;

    int n = 0;
    const cJSON* item = NULL;
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "nftables");
    cJSON_ArrayForEach(item, list)
    {
        const cJSON* set = cJSON_GetObjectItemCaseSensitive(item, "set");
        if (set == NULL) continue;

        const cJSON* name = cJSON_GetObjectItemCaseSensitive(set, "name");
        if (name == NULL || cJSON_IsString(name) == false || name->valuestring == NULL) continue;
        if (strcmp(name->valuestring, SMART_ACCT_SET_DL) != 0) continue;

        const cJSON* elem = NULL;
        const cJSON* elems = cJSON_GetObjectItemCaseSensitive(set, "elem");
        cJSON_ArrayForEach(elem, elems)
        {
            if (n >= max) break;

            const cJSON* one = cJSON_GetObjectItemCaseSensitive(elem, "elem");
            if (one == NULL) continue;

            const cJSON* val = cJSON_GetObjectItemCaseSensitive(one, "val");
            const cJSON* counter = cJSON_GetObjectItemCaseSensitive(one, "counter");
            if (val == NULL || cJSON_IsString(val) == false || val->valuestring == NULL) continue;
            if (counter == NULL) continue;

            const cJSON* bytes = cJSON_GetObjectItemCaseSensitive(counter, "bytes");
            if (bytes == NULL || cJSON_IsNumber(bytes) == false) continue;

            if (strlen(val->valuestring) >= IP_LEN) continue;
            copy_ip(out[n].ip, sizeof(out[n].ip), val->valuestring);
            out[n].bytes = (uint64_t)bytes->valuedouble;
            out[n].bps = 0;
            n++;
        }
        break;
    }

    cJSON_Delete(root);
    return n;
}

static bool acct_read(dev_rate_t* out, int* out_n)
{
    const char* nft = nft_tool();
    if (nft == NULL) return false;

    static char buf[SMART_CMD_BUF];
    char* argv[] = {(char*)nft, "-j", "list", "set", "ip", "esurf_demand", (char*)SMART_ACCT_SET_DL, NULL};
    if (run_capture(argv, buf, sizeof(buf)) != 0) return false;

    const int n = acct_parse(buf, out, SMART_ACCT_MAX);
    if (n < 0) return false;

    *out_n = n;
    return true;
}

static void heavy_clear(const char* why)
{
    if (s_heavy[0] == '\0') return;

    LOG_WARN("智能多播: 取消设备 %s 的多路独占 (%s)", s_heavy, why != NULL ? why : "不再需要");
    s_heavy[0] = '\0';
    s_heavy_cand[0] = '\0';
    s_heavy_cand_n = 0;
    s_heavy_idle_n = 0;
}

/**
 * @brief 挑出"下载最猛的那台设备"
 *
 * 判定要有迟滞, 不能这一秒是谁就给谁:
 * - 变成重设备要连续 SMART_HEAVY_STREAK 次采样都是同一个 IP
 * - 换设备要求新候选连续确认, 而且速率要明显高于当前重设备 (1.5 倍)
 * - 速率掉到阈值一半以下连续 SMART_HEAVY_IDLE_STREAK 次才取消
 * 这样避免设备在阈值附近抖动时反复改规则 (每次改规则都可能影响在跑的连接)。
 */
static void heavy_update(const dev_rate_t* cur, const int n)
{
    const uint64_t threshold = (uint64_t)g_demand_mbps * 1000000ULL / 8ULL;

    uint64_t best = 0;
    const char* best_ip = NULL;
    for (int i = 0; i < n; i++)
    {
        if (cur[i].bps > best)
        {
            best = cur[i].bps;
            best_ip = cur[i].ip;
        }
    }

    if (s_heavy[0] != '\0')
    {
        uint64_t mine = 0;
        for (int i = 0; i < n; i++)
        {
            if (strcmp(cur[i].ip, s_heavy) == 0)
            {
                mine = cur[i].bps;
                break;
            }
        }

        if (mine < threshold / 2)
        {
            s_heavy_idle_n++;
            if (s_heavy_idle_n >= SMART_HEAVY_IDLE_STREAK)
            {
                heavy_clear("下行已回落到阈值一半以下");
                return;
            }
        }
        else
        {
            s_heavy_idle_n = 0;
        }

        if (best_ip != NULL && strcmp(best_ip, s_heavy) != 0 && best >= threshold && best >= mine + mine / 2)
        {
            if (strcmp(s_heavy_cand, best_ip) == 0) s_heavy_cand_n++;
            else
            {
                copy_ip(s_heavy_cand, sizeof(s_heavy_cand), best_ip);
                s_heavy_cand_n = 1;
            }

            if (s_heavy_cand_n >= SMART_HEAVY_STREAK)
            {
                LOG_INFO("智能多播: 重设备由 %s 换成 %s (下行约 %" PRIu64 " Mbps)",
                    s_heavy, best_ip, (best * 8ULL) / 1000000ULL);
                copy_ip(s_heavy, sizeof(s_heavy), best_ip);
                s_heavy_cand[0] = '\0';
                s_heavy_cand_n = 0;
                s_heavy_idle_n = 0;
            }
        }
        else
        {
            s_heavy_cand[0] = '\0';
            s_heavy_cand_n = 0;
        }
        return;
    }

    if (best_ip != NULL && best >= threshold)
    {
        if (strcmp(s_heavy_cand, best_ip) == 0) s_heavy_cand_n++;
        else
        {
            copy_ip(s_heavy_cand, sizeof(s_heavy_cand), best_ip);
            s_heavy_cand_n = 1;
        }

        if (s_heavy_cand_n >= SMART_HEAVY_STREAK)
        {
            LOG_INFO("智能多播: 设备 %s 下行约 %" PRIu64 " Mbps, 让它独占多路",
                best_ip, (best * 8ULL) / 1000000ULL);
            copy_ip(s_heavy, sizeof(s_heavy), best_ip);
            s_heavy_cand[0] = '\0';
            s_heavy_cand_n = 0;
            s_heavy_idle_n = 0;
        }
        return;
    }

    s_heavy_cand[0] = '\0';
    s_heavy_cand_n = 0;
}

/**
 * @brief 每设备流量采样 (只在智能多播且至少两条线路在线时才做)
 *
 * 采样要 fork 一次 nft, 所以有空载成本: 一条线路在线时根本不采,
 * 靠 nft 集合里的累计计数器, 第二条线一上来立刻就能算出速率。
 */
static void acct_sample(const uint64_t now)
{
    if (g_smart_dial == false || g_demand_dial == false) return;
    if (s_dev_sample_ms != 0 && now >= s_dev_sample_ms && now - s_dev_sample_ms < SMART_ACCT_SAMPLE_MS) return;

    const uint64_t prev_ms = s_dev_sample_ms;
    s_dev_sample_ms = now;

    s_online_now = online_count();
    const int my = my_index();
    const bool primary = my >= 0 && my == primary_online_index();

    if (s_online_now < 2)
    {
        if (primary)
        {
            if (s_heavy[0] != '\0') LOG_INFO("智能多播: 线路回到 1 条, 取消重设备多路独占");
            heavy_publish("-", now);
        }
        s_heavy[0] = '\0';
        s_heavy_cand[0] = '\0';
        s_heavy_cand_n = 0;
        s_heavy_idle_n = 0;
        s_dev_prev_n = 0;
        return;
    }

    /**
     * 只让排在最前面的那条在线线路统计: 各进程采样时刻不同, 都算的话
     * 会得出不同的"重设备", 转发规则就会在两个结果之间来回改
     */
    if (primary == false) return;

    dev_rate_t cur[SMART_ACCT_MAX];
    int n = 0;
    if (acct_read(cur, &n) == false)
    {
        if (s_acct_warned == false)
        {
            LOG_WARN("智能多播: 读不到每设备流量计数 (需要 nft), 暂不做重设备多路独占, 扩容与收回不受影响");
            s_acct_warned = true;
        }
        heavy_clear("计数不可用");
        heavy_publish("-", now);
        s_dev_prev_n = 0;

        /* nft 在, 但读不到表: 多半是规则表被人删了, 下次强制重下一遍 */
        if (nft_tool() != NULL) s_steer_dirty = true;
        return;
    }
    s_acct_warned = false;

    const uint64_t dt = (prev_ms != 0 && now > prev_ms) ? (now - prev_ms) : 0;
    for (int i = 0; i < n; i++)
    {
        cur[i].bps = 0;
        if (dt == 0) continue;

        for (int j = 0; j < s_dev_prev_n; j++)
        {
            if (strcmp(cur[i].ip, s_dev_prev[j].ip) != 0) continue;
            if (cur[i].bytes < s_dev_prev[j].bytes) break;
            cur[i].bps = (cur[i].bytes - s_dev_prev[j].bytes) * 1000ULL / dt;
            break;
        }
    }

    heavy_update(cur, n);
    heavy_publish(s_heavy, now);

    memcpy(s_dev_prev, cur, (size_t)n * sizeof(dev_rate_t));
    s_dev_prev_n = n;
}

static void demand_sample(void)
{
    const uint64_t now = get_cur_tm_ms();
    if (s_sample_ms != 0 && now >= s_sample_ms && now - s_sample_ms < 1000) return;
    s_sample_ms = now;

    /* 每设备流量与网口采样互不影响, 计数不可用时也不该拖累扩容判断 */
    acct_sample(now);

    uint64_t rx = 0;
    uint64_t tx = 0;
    if (read_netdev(&rx, &tx) == false)
    {
        if (s_rx_warned == false)
        {
            LOG_WARN("按需多拨读不到网口收发字节, 上下行按 0 计, 不会扩容");
            s_rx_warned = true;
        }
        return;
    }
    s_rx_warned = false;

    if (s_count > 0 && (rx < s_samples[s_count - 1].rx || tx < s_samples[s_count - 1].tx))
    {
        s_count = 0;
    }

    if (s_count == DEMAND_SAMPLES)
    {
        memmove(s_samples, s_samples + 1, (DEMAND_SAMPLES - 1) * sizeof(demand_sample_t));
        s_count--;
    }
    s_samples[s_count].ms = now;
    s_samples[s_count].rx = rx;
    s_samples[s_count].tx = tx;
    s_count++;

    if (g_smart_dial && g_demand_dial)
    {
        s_conns = read_conntrack_count();
        s_devs = read_device_count();
    }
    else
    {
        s_conns = -1;
        s_devs = -1;
    }
}

typedef enum
{
    RATE_DOWN = 0,
    RATE_UP = 1
} rate_dir_t;

static uint64_t sample_counter(const demand_sample_t* sample, const rate_dir_t dir)
{
    return dir == RATE_DOWN ? sample->rx : sample->tx;
}

/** 取最近 samples 个采样点之间的平均速率 (bps) */
static uint64_t window_bps(const uint64_t now, const int samples, const rate_dir_t dir)
{
    if (s_count < 2 || samples < 2) return 0;

    int start = s_count - samples;
    if (start < 0) start = 0;

    const demand_sample_t* oldest = &s_samples[start];
    const demand_sample_t* newest = &s_samples[s_count - 1];
    if (newest->ms <= oldest->ms) return 0;
    if (now > newest->ms && now - newest->ms > 30000) return 0;

    const uint64_t older = sample_counter(oldest, dir);
    const uint64_t newer = sample_counter(newest, dir);
    if (newer < older) return 0;

    return (newer - older) * 1000ULL / (newest->ms - oldest->ms);
}

/**
 * @brief 最近一秒的峰值速率
 *
 * 一条线的实测速度到不了配置里的满速 (单条校园网本身就封顶在阈值附近), 拿平均值去等
 * "满速"会一直停在单拨。所以扩容看最近几秒里最高的一次采样间速率。
 */
static uint64_t peak_bps(const uint64_t now, const rate_dir_t dir)
{
    uint64_t peak = 0;
    const int begin = s_count > 4 ? s_count - 4 : 0;

    for (int i = begin + 1; i < s_count; i++)
    {
        const demand_sample_t* older = &s_samples[i - 1];
        const demand_sample_t* newer = &s_samples[i];
        if (newer->ms <= older->ms) continue;
        if (now > newer->ms && now - newer->ms > 30000) continue;

        const uint64_t a = sample_counter(older, dir);
        const uint64_t b = sample_counter(newer, dir);
        if (b < a) continue;

        const uint64_t rate = (b - a) * 1000ULL / (newer->ms - older->ms);
        if (rate > peak) peak = rate;
    }
    return peak;
}

/** 平均速率 (收回判定用) */
static void current_rates(const uint64_t now, uint64_t* down, uint64_t* up)
{
    *down = window_bps(now, DEMAND_SAMPLES, RATE_DOWN);
    *up = window_bps(now, DEMAND_SAMPLES, RATE_UP);
}

/** 峰值速率 (扩容判定用) */
static void peak_rates(const uint64_t now, uint64_t* down, uint64_t* up)
{
    *down = peak_bps(now, RATE_DOWN);
    *up = peak_bps(now, RATE_UP);
}

/* ------------------------------------------------------------------
 * 转发标记改写
 * ------------------------------------------------------------------ */

static bool marks_equal(const uint32_t* a, const int a_n, const uint32_t* b, const int b_n, const uint32_t mask)
{
    if (a_n != b_n) return false;
    for (int i = 0; i < a_n; i++)
    {
        if ((a[i] & mask) != (b[i] & mask)) return false;
    }
    return true;
}

/**
 * @brief 用 nft 下发转发规则
 *
 * 整个文件是一次 nft 事务, 所以规则是"整套换掉"而不是先删后加:
 * 中间没有"规则不在"的窗口, 换规则不会让转发流量丢包。
 *
 * 规则分三部分:
 * 1) 每设备流量计数 (智能多播): 局域网 <-> 外网的转发按设备累计字节
 * 2) 未认证线路的标记改写: 没上线的线路不该分到流量
 * 3) 重设备多路独占: 除重设备外的局域网设备一律压回第一条在线线路
 */
static bool steer_apply_nft(const uint32_t target, const uint32_t* from, const int from_n,
                            const uint32_t mask, const char* heavy)
{
    const char* nft = nft_tool();
    if (nft == NULL) return false;

    char nets[SMART_MAX_LAN_NETS][SMART_LAN_CIDR_LEN];
    const int net_n = collect_lan_nets(nets, SMART_MAX_LAN_NETS);

    if (net_n == 0 && s_lan_warned == false)
    {
        LOG_WARN("智能多播: 没认出局域网网段, 设备级多路独占与每设备计数都不会生效");
        s_lan_warned = true;
    }
    else if (net_n > 0)
    {
        s_lan_warned = false;
    }

    char path[160];
    snprintf(path, sizeof(path), DEMAND_DIR "/steer.nft");
    FILE* fp = fopen(path, "w");
    if (fp == NULL) return false;

    fprintf(fp, "add table ip esurf_demand\n");
    fprintf(fp, "add set ip esurf_demand " SMART_ACCT_SET_LAN " { type ipv4_addr ; flags interval ; }\n");
    fprintf(fp, "add set ip esurf_demand " SMART_ACCT_SET_DL
                " { type ipv4_addr ; flags dynamic ; timeout 10m ; size %d ; }\n", SMART_ACCT_MAX);
    fprintf(fp, "add set ip esurf_demand " SMART_ACCT_SET_UL
                " { type ipv4_addr ; flags dynamic ; timeout 10m ; size %d ; }\n", SMART_ACCT_MAX);
    fprintf(fp, "add chain ip esurf_demand " SMART_ACCT_CHAIN
                " { type filter hook forward priority filter - 10 ; policy accept ; }\n");
    fprintf(fp, "add chain ip esurf_demand prerouting"
                " { type filter hook prerouting priority mangle + 1 ; policy accept ; }\n");

    /* 局域网网段按当前接口地址刷新; 集合元素没有计数器, 清掉重建没有代价 */
    fprintf(fp, "flush set ip esurf_demand " SMART_ACCT_SET_LAN "\n");
    if (net_n > 0)
    {
        fprintf(fp, "add element ip esurf_demand " SMART_ACCT_SET_LAN " { ");
        for (int i = 0; i < net_n; i++) fprintf(fp, "%s%s", i == 0 ? "" : ", ", nets[i]);
        fprintf(fp, " }\n");
    }

    /**
     * 计数规则只在智能多播时挂上; 链本身留空没有代价,
     * 这样开关智能多播不需要删链 (删链失败会把整批规则一起废掉)
     */
    fprintf(fp, "flush chain ip esurf_demand " SMART_ACCT_CHAIN "\n");
    if (g_smart_dial && g_demand_dial && net_n > 0)
    {
        fprintf(fp, "add rule ip esurf_demand " SMART_ACCT_CHAIN
                    " ip saddr != @" SMART_ACCT_SET_LAN " ip daddr @" SMART_ACCT_SET_LAN
                    " update @" SMART_ACCT_SET_DL " { ip daddr counter }\n");
        fprintf(fp, "add rule ip esurf_demand " SMART_ACCT_CHAIN
                    " ip saddr @" SMART_ACCT_SET_LAN " ip daddr != @" SMART_ACCT_SET_LAN
                    " update @" SMART_ACCT_SET_UL " { ip saddr counter }\n");
    }

    fprintf(fp, "flush chain ip esurf_demand prerouting\n");

    if (from_n > 0)
    {
        const uint32_t keep = ~mask;
        for (int i = 0; i < from_n; i++)
        {
            fprintf(fp,
                "add rule ip esurf_demand prerouting"
                " meta mark and 0x%" PRIx32 " == 0x%" PRIx32
                " meta mark set meta mark and 0x%" PRIx32 " or 0x%" PRIx32 "\n",
                mask, from[i] & mask, keep, target & mask);
        }
    }

    /**
     * 重设备多路独占:
     * - 只动"局域网 -> 外网"的包 (ip daddr != @lan_nets), 内网互访不受影响
     * - 不碰 OUTPUT: 认证进程自己发的包在 output 链上, 待机账号还能继续认证
     * - 只有在线线路 >= 2 条时调用方才传 heavy 进来
     */
    if (g_smart_dial && g_demand_dial && heavy != NULL && heavy[0] != '\0' && net_n > 0)
    {
        const uint32_t keep = ~mask;
        fprintf(fp,
            "add rule ip esurf_demand prerouting"
            " ip saddr @" SMART_ACCT_SET_LAN " ip saddr != %s ip daddr != @" SMART_ACCT_SET_LAN
            " meta mark set meta mark and 0x%" PRIx32 " or 0x%" PRIx32 "\n",
            heavy, keep, target & mask);
    }

    if (fclose(fp) != 0) return false;

    char* argv[] = {(char*)nft, "-f", path, NULL};
    return run_exec(argv) == 0;
}

static bool steer_apply_iptables(const uint32_t target, const uint32_t* from, const int from_n, const uint32_t mask)
{
    const char* iptables = find_tool("/usr/sbin/iptables", "/usr/bin/iptables");
    if (iptables == NULL) return false;

    char mark[64];
    char* del_argv[] = {(char*)iptables, "-t", "mangle", "-D", "PREROUTING", "-j", "esurf_demand", NULL};
    run_exec(del_argv);
    char* flush_argv[] = {(char*)iptables, "-t", "mangle", "-F", "esurf_demand", NULL};
    run_exec(flush_argv);
    char* free_argv[] = {(char*)iptables, "-t", "mangle", "-X", "esurf_demand", NULL};
    run_exec(free_argv);

    if (from_n == 0) return true;

    char* new_argv[] = {(char*)iptables, "-t", "mangle", "-N", "esurf_demand", NULL};
    if (run_exec(new_argv) != 0) return false;

    for (int i = 0; i < from_n; i++)
    {
        snprintf(mark, sizeof(mark), "0x%" PRIx32 "/0x%" PRIx32, from[i] & mask, mask);
        char xmark[64];
        snprintf(xmark, sizeof(xmark), "0x%" PRIx32 "/0x%" PRIx32, target & mask, mask);
        char* add_argv[] = {
            (char*)iptables, "-t", "mangle", "-A", "esurf_demand",
            "-m", "mark", "--mark", mark, "-j", "MARK", "--set-xmark", xmark, NULL
        };
        if (run_exec(add_argv) != 0) return false;
    }

    char* jump_argv[] = {(char*)iptables, "-t", "mangle", "-A", "PREROUTING", "-j", "esurf_demand", NULL};
    return run_exec(jump_argv) == 0;
}

/**
 * @brief 本进程是不是"规则写入者"
 *
 * 只有排在最前、状态文件还新鲜的那个认证进程改规则表。每个进程各记一份"已经应用的规则"
 * 再一起写的话, 后写的会按旧状态把刚上线的线路又改回第一条, 多拨测速就还是单线的速度。
 */
static bool steer_is_leader(void)
{
    const uint64_t now = get_cur_tm_ms();

    for (int i = 0; i < g_account_order_cnt; i++)
    {
        const uint8_t idx = g_account_order[i];
        if (idx == g_prog_account) return true;

        int pid = 0;
        int state = -1;
        uint64_t since = 0;
        uint64_t updated = 0;
        if (read_peer(idx, &pid, &state, &since, &updated) == false)
        {
            if (within(now, g_start_run_tm, 5000)) return false;
            continue;
        }
        if (pid_alive(pid) && within(now, updated, fresh_ms())) return false;
    }
    return true;
}

static void demand_steer(const bool force)
{
    static uint32_t applied_target = 0;
    static uint32_t applied_from[ACCOUNT_ORDER_MAX];
    static int applied_n = -1;
    static char applied_heavy[IP_LEN] = {0};
    static bool applied_smart = false;
    static bool applied_leader = false;
    static bool warned = false;

    if (steer_is_leader() == false)
    {
        /* 失去写入者身份: 下次轮到自己时强制重下一遍, 免得拿旧的缓存跳过 */
        applied_leader = false;
        return;
    }

    const bool became_leader = (applied_leader == false);
    applied_leader = true;

    /**
     * 没开按需多拨时不必建表: 之前没下发过就直接返回,
     * 下发过 (applied_n >= 0) 才走一遍把残留规则清掉
     */
    if (demand_enabled() == false && applied_n < 0) return;

    uint32_t target = 0;
    uint32_t from[ACCOUNT_ORDER_MAX];
    int from_n = 0;
    const char* heavy = NULL;

    if (demand_enabled())
    {
        const uint32_t mask = mwan_mask();
        for (int i = 0; i < g_account_order_cnt; i++)
        {
            if (g_account_mark[i] == 0) continue;
            if (account_is_online(i) == false) continue;
            target = g_account_mark[i];
            break;
        }

        if (target != 0)
        {
            for (int i = 0; i < g_account_order_cnt; i++)
            {
                const uint32_t mark = g_account_mark[i];
                if (mark == 0 || (mark & mask) == (target & mask)) continue;
                if (account_is_online(i)) continue;
                if (from_n < ACCOUNT_ORDER_MAX) from[from_n++] = mark;
            }

            /**
             * 多路独占: 只有两条以上线路在线才谈得上"多路",
             * 重设备由第一条在线线路统计后写在共享状态文件里, 这里只读
             */
            if (g_smart_dial) heavy = heavy_device();
        }
    }

    const uint32_t mask = mwan_mask();
    const bool smart_now = g_smart_dial && g_demand_dial;
    const bool dirty = s_steer_dirty || became_leader;
    s_steer_dirty = false;

    if (force == false && dirty == false && applied_n >= 0 &&
        (applied_target & mask) == (target & mask) &&
        applied_smart == smart_now &&
        strcmp(applied_heavy, safe_str(heavy)) == 0 &&
        marks_equal(applied_from, applied_n, from, from_n, mask))
    {
        return;
    }

    ensure_dir();
    const int lock_fd = open(DEMAND_DIR "/steer.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) return;
    if (flock(lock_fd, LOCK_EX) != 0)
    {
        close(lock_fd);
        return;
    }

    const bool grew = applied_n >= 0 && from_n != applied_n;
    const bool ok = steer_apply_nft(target, from, from_n, mask, heavy) ||
                    steer_apply_iptables(target, from, from_n, mask);
    if (ok == false)
    {
        if (warned == false)
        {
            LOG_WARN("按需多拨: 无法改写转发标记, 未认证的线路仍会被 mwan3 分到流量");
            warned = true;
        }
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return;
    }

    warned = false;
    if (from_n == 0)
    {
        if (applied_n > 0) LOG_INFO("按需多拨: 转发恢复由 mwan3 分配");
    }
    else
    {
        LOG_INFO("按需多拨: 未认证线路的转发改走到标记 0x%" PRIx32, target);
    }

    if (heavy != NULL && heavy[0] != '\0')
    {
        if (strcmp(applied_heavy, heavy) != 0)
        {
            LOG_INFO("智能多播: 设备 %s 独占多路, 其它设备转发压回标记 0x%" PRIx32, heavy, target & mask);
        }
    }
    else if (applied_heavy[0] != '\0')
    {
        LOG_INFO("智能多播: 取消设备级多路独占, 转发交给 mwan3 分配");
    }

    applied_target = target;
    applied_n = from_n;
    applied_smart = smart_now;
    copy_ip(applied_heavy, sizeof(applied_heavy), safe_str(heavy));
    for (int i = 0; i < from_n; i++) applied_from[i] = from[i];

    /**
     * 在线线路的集合一变就清一次连接跟踪: 新线路拨上时, 已经粘在旧线路上的连接得重连
     * 才会分到新线; 有线路下线时, 那些还指着它的连接也得重来。设备级独占的增删不在这里,
     * 刻意不清, 免得把正在跑的连接打断。
     */
    if (grew) flush_conntrack();

    flock(lock_fd, LOCK_UN);
    close(lock_fd);
}

/* ------------------------------------------------------------------
 * 扩容 / 收回判定
 * ------------------------------------------------------------------ */

static void classify(const uint64_t now, const int my, int* online_ahead, int* blocking)
{
    *online_ahead = 0;
    *blocking = 0;

    for (int i = 0; i < my; i++)
    {
        const uint8_t idx = g_account_order[i];
        if (idx == g_prog_account) continue;

        int pid = 0;
        int state = -1;
        uint64_t since = 0;
        uint64_t updated = 0;
        if (read_peer(idx, &pid, &state, &since, &updated) == false)
        {
            if (within(now, g_start_run_tm, DEMAND_STUCK_MS)) (*blocking)++;
            continue;
        }

        const bool alive = pid_alive(pid) && within(now, updated, fresh_ms());
        if (alive == false)
        {
            if (within(now, updated, DEMAND_STUCK_MS)) (*blocking)++;
            continue;
        }

        switch (state)
        {
        case DEMAND_OFF:
            break;
        case DEMAND_STANDBY:
            (*blocking)++;
            break;
        case DEMAND_AUTH:
            if (within(now, since, DEMAND_STUCK_MS) == false) break;
            (*blocking)++;
            break;
        case DEMAND_ONLINE:
            (*online_ahead)++;
            break;
        default:
            if (within(now, g_start_run_tm, DEMAND_STUCK_MS)) (*blocking)++;
            break;
        }
    }
}

/** 条件连续成立 need_ms 之后才算数 */
static bool held_for(uint64_t* since, const uint64_t now, const bool below, const uint64_t need_ms)
{
    if (below == false)
    {
        *since = 0;
        return false;
    }
    if (*since == 0) *since = now;
    return now - *since >= need_ms;
}

/** 上一次扩容 / 收回之后还在静默期里吗 */
static bool in_hold_off(const uint64_t now)
{
    if (g_smart_hold_secs == 0) return false;
    if (s_last_change_ms == 0) return false;
    if (now < s_last_change_ms) return false;
    return now - s_last_change_ms < (uint64_t)g_smart_hold_secs * 1000ULL;
}

/**
 * @brief 该不该扩容 / 收回
 *
 * 四条依据 (智能多播开的时候): 下行、上行、连接数、在线设备数,
 * 只要有一条到"本账号前面已有 N 条在线线路"对应的阈值就该再来一条。
 *
 * 连接数与设备数只在确实有流量时才算数, 而且收回时也只在有流量时才用它们兜底,
 * 否则一堆设备挂着不动就会一直占着多条线路不放。
 */
static demand_act_t decide(const bool online, const bool authing)
{
    const int my = my_index();
    if (my < 0)
    {
        if (s_rank_warned == false)
        {
            LOG_WARN("按需多拨找不到本账号的排位, 本账号照常认证");
            s_rank_warned = true;
        }
        return ACT_GO;
    }

    const uint64_t now = get_cur_tm_ms();
    int ahead = 0;
    int blocking = 0;
    classify(now, my, &ahead, &blocking);

    uint64_t down = 0;
    uint64_t up = 0;
    uint64_t peak_down = 0;
    uint64_t peak_up = 0;
    current_rates(now, &down, &up);
    peak_rates(now, &peak_down, &peak_up);

    const bool smart = g_smart_dial;
    const uint64_t need_down = (uint64_t)ahead * (uint64_t)g_demand_mbps * 1000000ULL / 8ULL;
    const uint64_t need_up = (uint64_t)ahead * (uint64_t)g_demand_up_mbps * 1000000ULL / 8ULL;
    const uint64_t need_conns = (uint64_t)ahead * (uint64_t)g_smart_conns;
    const uint64_t need_devs = (uint64_t)ahead * (uint64_t)g_smart_devices;

    /**
     * 扩容看"半条线": 单条线的实测速度到不了配置里的满速 (校园网本身就封顶在阈值附近),
     * 拿平均值去等满速会一直停在单拨。收回仍然看平均值, 免得一次突发就把线路留住。
     */
    const uint64_t rise_down = need_down / 2;
    const uint64_t rise_up = need_up / 2;
    const uint64_t low_down = need_down / 2;
    const uint64_t low_up = need_up / 2;

    /* "确实有流量"的门槛: 下行阈值的 1/4 */
    const uint64_t floor_bps = (uint64_t)g_demand_mbps * 1000000ULL / 8ULL / 4ULL;
    const bool active = (down + up) >= floor_bps;

    const bool use_conns = smart && active && s_conns >= 0 && g_smart_conns > 0;
    const bool use_devs = smart && active && s_devs >= 0 && g_smart_devices > 0;

    bool high = (rise_down > 0 && peak_down >= rise_down);
    if (smart && rise_up > 0 && peak_up >= rise_up) high = true;
    if (use_conns && (uint64_t)s_conns >= need_conns) high = true;
    if (use_devs && (uint64_t)s_devs >= need_devs) high = true;

    bool low = down < low_down;
    if (smart && up >= low_up) low = false;
    if (use_conns && (uint64_t)s_conns >= need_conns / 2) low = false;
    if (use_devs && (uint64_t)s_devs >= need_devs / 2) low = false;

    s_dbg_ahead = ahead;
    s_dbg_blocking = blocking;
    s_dbg_bps = peak_down;
    s_dbg_up_bps = peak_up;
    s_dbg_conns = s_conns;
    s_dbg_devs = s_devs;
    s_dbg_high = high;
    s_dbg_low = low;

    if (ahead == 0 && blocking == 0)
    {
        s_low_since = 0;
        s_auth_low_since = 0;
        return ACT_GO;
    }

    if (online)
    {
        if (ahead == 0)
        {
            s_low_since = 0;
            return ACT_HOLD;
        }
        if (in_hold_off(now))
        {
            s_low_since = 0;
            return ACT_HOLD;
        }
        if (held_for(&s_low_since, now, low, (uint64_t)g_demand_idle_mins * 60000ULL))
        {
            s_last_change_ms = now;
            return ACT_STOP;
        }
        return ACT_HOLD;
    }

    if (authing)
    {
        if (blocking > 0) return ACT_STOP;
        if (held_for(&s_auth_low_since, now, low, DEMAND_AUTH_ABORT_MS))
        {
            s_last_change_ms = now;
            return ACT_STOP;
        }
        return ACT_HOLD;
    }

    if (blocking == 0 && ahead > 0 && high && in_hold_off(now) == false)
    {
        s_last_change_ms = now;
        return ACT_GO;
    }
    return ACT_HOLD;
}

static void log_snapshot(const char* what)
{
    LOG_INFO("按需多拨: %s (前面在线 %d 个, 挡路 %d 个, 最近一秒峰值下行约 %" PRIu64 " Mbps, 上行约 %" PRIu64
             " Mbps, 连接 %d, 设备 %d, 判定: %s%s)",
        what, s_dbg_ahead, s_dbg_blocking,
        (s_dbg_bps * 8ULL) / 1000000ULL, (s_dbg_up_bps * 8ULL) / 1000000ULL,
        s_dbg_conns, s_dbg_devs,
        s_dbg_high ? "到扩容线" : "-", s_dbg_low ? " 到收回线" : "");
}

bool demand_admit(void)
{
    if (demand_enabled() == false) return true;

    demand_sample();
    const bool go = decide(false, false) == ACT_GO;
    if ((int)go != s_last_admit)
    {
        if (go == false)
        {
            log_snapshot(g_smart_dial ? "待机, 下行/上行/连接数/设备数都没到扩容线" : "待机, 下载量未到扩容阈值");
        }
        else if (s_last_admit == 0)
        {
            log_snapshot("达到阈值, 开始认证");
        }
        s_last_admit = (int)go;
    }
    return go;
}

bool demand_yield(const bool online)
{
    if (demand_enabled() == false) return false;

    demand_sample();
    if (decide(online, online == false) != ACT_STOP)
    {
        s_yield_logged = false;
        return false;
    }

    if (s_yield_logged == false)
    {
        log_snapshot(online ? "负载低于收回线, 登出并待机" : "不再需要本账号, 放弃这次认证");
        s_yield_logged = true;
    }
    return true;
}

void demand_touch(void)
{
    if (tl_thread_name != NULL && strcmp(tl_thread_name, "watchdog") == 0) return;
    if (demand_enabled() == false || s_state < 0) return;

    const uint64_t now = get_cur_tm_ms();
    if (s_touch_ms != 0 && now >= s_touch_ms && now - s_touch_ms < 1000) return;
    s_touch_ms = now;

    demand_sample();
    write_file(now);
    demand_steer(false);

    if (s_state != DEMAND_AUTH && s_state != DEMAND_ONLINE) return;
    if (tl_thread_idx < 0 || g_prog_status == NULL) return;
    if (decide(s_state == DEMAND_ONLINE, s_state == DEMAND_AUTH) != ACT_STOP) return;

    g_prog_status[tl_thread_idx].runtime_status.is_need_reauth = true;
}
