#include "utils/DemandDial.h"

#include "states/States.h"

#include "utils/PlatformUtils.h"
#include "utils/Logger.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#define DEMAND_DIR "/var/run/esurfingclient"

#define DEMAND_SAMPLES 10

#define DEMAND_FRESH_MS 15000

#define DEMAND_STUCK_MS 120000

#define DEMAND_AUTH_ABORT_MS 30000

#define DEMAND_IFACE_MAX 8

#define DEMAND_IFACE_NAME 16

typedef struct
{
    uint64_t ms;
    uint64_t bytes;
} demand_sample_t;

static int s_state = -1;
static uint64_t s_since = 0;
static uint64_t s_touch_ms = 0;
static uint64_t s_low_since = 0;
static uint64_t s_auth_low_since = 0;

static demand_sample_t s_samples[DEMAND_SAMPLES];
static int s_count = 0;
static uint64_t s_sample_ms = 0;

static int s_dbg_ahead = 0;
static int s_dbg_blocking = 0;
static uint64_t s_dbg_bps = 0;

static int s_last_admit = -1;
static bool s_yield_logged = false;
static bool s_rank_warned = false;
static bool s_rx_warned = false;
static bool s_dir_warned = false;
static char s_iface_logged[DEMAND_IFACE_LEN] = {0};

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
}

void demand_clear(void)
{
    s_state = -1;
    if (g_prog_account == 0) return;

    char path[160];
    if (state_path(path, sizeof(path), g_prog_account, false) == false) return;
    unlink(path);
}

void demand_banner(void)
{
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

    LOG_INFO("按需多拨已开启, 本账号排位 %d/%" PRIu8 ", 阈值 %" PRIu32 " Mbps, 连续 %" PRIu32
             " 分钟低于一半则收回, 统计网口: %s",
        idx + 1, g_account_order_cnt, g_demand_mbps, g_demand_idle_mins, iface);
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
        LOG_WARN("按需多拨没有可统计的网口 (默认路由或 demand_iface), 下行按 0 计, 不会扩容");
        return;
    }
    LOG_INFO("按需多拨统计网口: %s", joined);
}

static bool read_rx(uint64_t* bytes_out)
{
    char names[DEMAND_IFACE_MAX][DEMAND_IFACE_NAME];
    const int count = collect_ifaces(names);
    log_ifaces(names, count);
    if (count == 0)
    {
        *bytes_out = 0;
        return false;
    }

    FILE* fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) return false;

    uint64_t sum = 0;
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

        unsigned long long rx = 0;
        if (sscanf(colon + 1, " %llu", &rx) != 1) continue;
        sum += (uint64_t)rx;
        matched++;
    }
    fclose(fp);

    if (matched == 0) return false;
    *bytes_out = sum;
    return true;
}

static void demand_sample(void)
{
    const uint64_t now = get_cur_tm_ms();
    if (s_sample_ms != 0 && now >= s_sample_ms && now - s_sample_ms < 1000) return;
    s_sample_ms = now;

    uint64_t bytes = 0;
    if (read_rx(&bytes) == false)
    {
        if (s_rx_warned == false)
        {
            LOG_WARN("按需多拨读不到网口接收字节, 下行按 0 计, 不会扩容");
            s_rx_warned = true;
        }
        return;
    }
    s_rx_warned = false;

    if (s_count > 0 && bytes < s_samples[s_count - 1].bytes)
    {
        s_count = 0;
    }

    if (s_count == DEMAND_SAMPLES)
    {
        memmove(s_samples, s_samples + 1, (DEMAND_SAMPLES - 1) * sizeof(demand_sample_t));
        s_count--;
    }
    s_samples[s_count].ms = now;
    s_samples[s_count].bytes = bytes;
    s_count++;
}

static uint64_t current_bps(const uint64_t now)
{
    if (s_count < 2) return 0;

    const demand_sample_t* oldest = &s_samples[0];
    const demand_sample_t* newest = &s_samples[s_count - 1];
    if (newest->ms <= oldest->ms) return 0;
    if (now > newest->ms && now - newest->ms > 30000) return 0;
    if (newest->bytes < oldest->bytes) return 0;

    return (newest->bytes - oldest->bytes) * 1000ULL / (newest->ms - oldest->ms);
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

static bool below_for(uint64_t* since, const uint64_t now, const uint64_t bps, const uint64_t low, const uint64_t need_ms)
{
    if (bps >= low)
    {
        *since = 0;
        return false;
    }
    if (*since == 0) *since = now;
    return now - *since >= need_ms;
}

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

    const uint64_t bps = current_bps(now);
    const uint64_t high = (uint64_t)ahead * (uint64_t)g_demand_mbps * 1000000ULL / 8ULL;
    const uint64_t low = high / 2;

    s_dbg_ahead = ahead;
    s_dbg_blocking = blocking;
    s_dbg_bps = bps;

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
        if (below_for(&s_low_since, now, bps, low, (uint64_t)g_demand_idle_mins * 60000ULL)) return ACT_STOP;
        return ACT_HOLD;
    }

    if (authing)
    {
        if (blocking > 0) return ACT_STOP;
        if (below_for(&s_auth_low_since, now, bps, low, DEMAND_AUTH_ABORT_MS)) return ACT_STOP;
        return ACT_HOLD;
    }

    if (blocking == 0 && ahead > 0 && high > 0 && bps >= high) return ACT_GO;
    return ACT_HOLD;
}

static void log_snapshot(const char* what)
{
    LOG_INFO("按需多拨: %s (前面在线 %d 个, 挡路 %d 个, 近 10 秒下行约 %" PRIu64 " Mbps)",
        what, s_dbg_ahead, s_dbg_blocking, (s_dbg_bps * 8ULL) / 1000000ULL);
}

bool demand_admit(void)
{
    if (demand_enabled() == false) return true;

    demand_sample();
    const bool go = decide(false, false) == ACT_GO;
    if ((int)go != s_last_admit)
    {
        if (go == false) log_snapshot("待机, 下载量未到扩容阈值");
        else if (s_last_admit == 0) log_snapshot("达到阈值, 开始认证");
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
        log_snapshot(online ? "下行低于收回线, 登出并待机" : "不再需要本账号, 放弃这次认证");
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

    if (s_state != DEMAND_AUTH && s_state != DEMAND_ONLINE) return;
    if (tl_thread_idx < 0 || g_prog_status == NULL) return;
    if (decide(s_state == DEMAND_ONLINE, s_state == DEMAND_AUTH) != ACT_STOP) return;

    g_prog_status[tl_thread_idx].runtime_status.is_need_reauth = true;
}
