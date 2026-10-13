/* 独立进程级规则缓存回归; 与 test_demand_dial.c 用相同的桩和编译参数。 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int s_fake_exit = 1;
static int test_execvp(const char* file, char* const argv[])
{
    FILE* fp = fopen(DEMAND_DIR "/exec.calls", "a");
    if (fp == NULL) _exit(126);
    fprintf(fp, "%s", file);
    for (int i = 1; argv[i] != NULL; i++) fprintf(fp, " %s", argv[i]);
    fputc('\n', fp);
    fclose(fp);
    _exit(s_fake_exit);
}

#define execvp test_execvp
#define main decision_test_main
#include "test_demand_dial.c"
#undef main
#undef execvp

static int calls(void)
{
    FILE* fp = fopen(DEMAND_DIR "/exec.calls", "r");
    if (fp == NULL) return 0;
    int n = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) n++;
    fclose(fp);
    return n;
}

int main(void)
{
    ensure_dir();
    unlink(DEMAND_DIR "/exec.calls");
    g_account_order_cnt = 3;
    for (int i = 0; i < 3; i++)
    {
        g_account_order[i] = i + 1;
        g_account_mark[i] = (i + 1) * 0x100;
    }
    g_prog_account = 1;
    g_start_run_tm = g_test_now - 200000;
    s_state = DEMAND_ONLINE;
    write_peer_state(2, DEMAND_STANDBY);
    write_peer_state(3, DEMAND_ONLINE);
    demand_steer(false);
    const int first = calls();
    check_bool("首次失败确实调用外部工具", first > 0, true);
    g_test_now += 1000;
    demand_steer(false);
    check_bool("同样的失败规则处于退避期", calls() == first, true);
    write_peer_state(2, DEMAND_ONLINE);
    write_peer_state(3, DEMAND_STANDBY);
    demand_steer(false);
    check_bool("离线mark数量相同但内容变了立即重试", calls() > first, true);
    const int second = calls();
    s_fake_exit = 0;
    g_test_now += DEMAND_APPLY_RETRY_MS;
    write_peer_state(2, DEMAND_STANDBY);
    write_peer_state(3, DEMAND_STANDBY);
    demand_steer(false);
    check_bool("退避后能成功补下规则", calls() > second, true);
    const int applied = calls();
    g_test_now += 1000;
    demand_steer(false);
    check_bool("规则未变化不每秒fork", calls() == applied, true);
    g_test_now += DEMAND_REFRESH_MS;
    write_peer_state(2, DEMAND_STANDBY);
    write_peer_state(3, DEMAND_STANDBY);
    demand_steer(false);
    check_bool("仅一条线路也定期补规则, 避免删表后永久失效", calls() > applied, true);
    remove_peer_state(2);
    remove_peer_state(3);
    demand_clear();
    printf("规则缓存回归: 通过 %d 项, 失败 %d 项\n", s_pass, s_fail);
    return s_fail != 0;
}
