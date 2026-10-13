#!/bin/sh
# Linux 回归入口。--network 额外运行真实双 WAN/NAT 抓包 (需 ip/nft/tcpdump/nsenter)。
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d /tmp/esurf-demand-test.XXXXXX)
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
CC=${CC:-gcc}
APP="$ROOT/esurfingclient/app"

for name in demand_dial demand_cache; do
    "$CC" -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -DDEMAND_DIR="\"$OUT/$name-state\"" \
        -I "$APP/include" -I "$APP/src" -I "$APP/third_party" \
        "$APP/test/test_$name.c" "$APP/third_party/cJSON/cJSON.c" -o "$OUT/test_$name"
    # 不在主机网络命名空间加载测试规则, 普通用户通过 user namespace 获取权限。
    unshare -rn --map-root-user sh -c '
        set -e
        ip link add lan type dummy
        ip addr add 192.168.9.1/24 dev lan
        ip link set lan up
        cd "$1"
        DEMAND_TEST_REQUIRE_NFT=1 exec "$2"
    ' sh "$OUT" "$OUT/test_$name"
done

"$CC" -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$APP/include" -I "$APP/src" -I "$APP/third_party" \
    "$APP/test/test_resolve_url.c" -o "$OUT/test_resolve_url"
"$OUT/test_resolve_url"

if [ "${1:-}" = "--network" ]; then
    "$CC" -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
        -DDEMAND_DIR="\"$OUT/network-state\"" \
        -I "$APP/include" -I "$APP/src" -I "$APP/third_party" \
        "$APP/test/test_demand_dial.c" "$APP/third_party/cJSON/cJSON.c" -o "$OUT/generator"
    unshare -rn --map-root-user python3 "$APP/test/test_demand_network.py" "$OUT/generator" "$OUT/network"
fi
printf '\nAll requested demand-dial regression tests passed.\n'
