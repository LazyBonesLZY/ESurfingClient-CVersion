#!/usr/bin/env python3
"""真实双出口/NAT 回归。只在隔离的 netns 中执行，不操作主机规则。

运行: unshare -rn --map-root-user python3 test_demand_network.py GENERATOR OUTPUT_DIR
GENERATOR 是 test_demand_dial.c 编译的可执行文件；依赖 ip/nft/tcpdump/nsenter。
模拟 mwan3 的 restore/classify/save 顺序，不替代目标路由器吞吐/offload 测试。
"""
import os
import signal
import subprocess as sp
import sys
import time
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: test_demand_network.py GENERATOR OUTPUT_DIR")
if os.readlink('/proc/self/ns/net') == os.readlink('/proc/1/ns/net'):
    raise SystemExit("Run inside a separate network namespace")
generator = str(Path(sys.argv[1]).resolve())
root = Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=True)
children = []


def run(*args):
    result = sp.run(args, text=True, stdout=sp.PIPE, stderr=sp.PIPE)
    if result.returncode:
        raise RuntimeError(f"{args}: {result.stderr}")
    return result.stdout


def namespace():
    p = sp.Popen(['unshare', '-n', 'sleep', '90'])
    children.append(p)
    time.sleep(.15)
    return p.pid


def nrun(pid, *args):
    return run('nsenter', '-t', str(pid), '-n', *args)


def python_in(pid, code, **kwargs):
    p = sp.Popen(['nsenter', '-t', str(pid), '-n', 'python3', '-u', '-c', code], **kwargs)
    children.append(p)
    return p


try:
    client, server = namespace(), namespace()
    for local, remote, pid in [('lan', 'cl', client), ('wan1', 'sv1', server), ('wan2', 'sv2', server)]:
        run('ip', 'link', 'add', local, 'type', 'veth', 'peer', 'name', remote)
        run('ip', 'link', 'set', remote, 'netns', str(pid))
        run('ip', 'link', 'set', local, 'up')
        nrun(pid, 'ip', 'link', 'set', remote, 'up')
    for addr, dev in [('192.168.9.1/24', 'lan'), ('10.1.0.1/24', 'wan1'), ('10.2.0.1/24', 'wan2')]:
        run('ip', 'addr', 'add', addr, 'dev', dev)
    nrun(client, 'ip', 'addr', 'add', '192.168.9.10/24', 'dev', 'cl')
    nrun(client, 'ip', 'route', 'add', 'default', 'via', '192.168.9.1')
    for addr, dev in [('10.1.0.2/24', 'sv1'), ('10.2.0.2/24', 'sv2'), ('203.0.113.10/32', 'lo')]:
        nrun(server, 'ip', 'addr', 'add', addr, 'dev', dev)
    nrun(server, 'ip', 'link', 'set', 'lo', 'up')
    nrun(server, 'sysctl', '-qw', 'net.ipv4.conf.all.rp_filter=0')
    for dev in ['all', 'default', 'lan', 'wan1', 'wan2']:
        run('sysctl', '-qw', 'net.ipv4.conf.' + dev + '.rp_filter=0')
    run('sysctl', '-qw', 'net.ipv4.ip_forward=1')
    for table, gateway, dev, mark in [('101', '10.1.0.2', 'wan1', '0x100'), ('102', '10.2.0.2', 'wan2', '0x200')]:
        run('ip', 'route', 'add', 'table', table, '192.168.9.0/24', 'dev', 'lan')
        run('ip', 'route', 'add', 'table', table, 'default', 'via', gateway, 'dev', dev)
        run('ip', 'rule', 'add', 'pref', table, 'fwmark', mark + '/0x3f00', 'lookup', table)
    run('ip', 'route', 'add', '203.0.113.10/32', 'via', '10.2.0.2', 'dev', 'wan2')
    sp.run(['nft', '-f', '-'], input='''table ip lab {
 chain mwan { type filter hook prerouting priority -150; policy accept;
 iifname "lan" meta nftrace set 1
 iifname "lan" meta mark set ct mark
 iifname "lan" meta mark and 0x3f00 == 0 meta mark set 0x80004200
 iifname "lan" ct mark set meta mark
 }
 chain post { type nat hook postrouting priority 100; policy accept;
 oifname "wan1" snat to 10.1.0.1
 oifname "wan2" snat to 10.2.0.1
 }
}''', text=True, check=True)
    tracefile = open(root / 'trace.txt', 'w')
    trace = sp.Popen(['nft', 'monitor', 'trace'], stdout=tracefile)
    children.append(trace)
    python_in(server, 'import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(("203.0.113.10",9000));\nwhile True:\n d,a=s.recvfrom(2000);\n if not d.startswith(b"noreply"): s.sendto(b"ACK",a)')
    captures = []
    for dev in ['wan1', 'wan2']:
        f = open(root / (dev + '.pcap'), 'wb')
        p = sp.Popen(['tcpdump', '-Z', 'root', '--immediate-mode', '-U', '-n', '-i', dev, '-w', '-', 'udp port 9000'], stdout=f, stderr=sp.DEVNULL)
        children.append(p)
        captures.append((p, f))
    sender = python_in(client, 'import socket,sys; ss={}\nfor l in sys.stdin:\n port,data=l.strip().split(" ",1);port=int(port);\n if port not in ss: ss[port]=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);ss[port].bind(("192.168.9.10",port))\n ss[port].sendto(data.encode(),("203.0.113.10",9000))', stdin=sp.PIPE, text=True)
    time.sleep(.4)

    def send(port, data):
        sender.stdin.write(str(port) + ' ' + data + '\n')
        sender.stdin.flush()
        time.sleep(.4)

    send(34567, 'old-before-pin')
    send(34568, 'noreply-old-before-pin')
    run(generator, '--generate-rules', '192.168.9.66')
    send(34567, 'old-after-pin')
    send(34568, 'noreply-old-after-pin')
    send(34569, 'new-pinned')
    send(34569, 'new-pinned-second')
    run(generator, '--generate-rules', 'none')
    send(34569, 'pinned-after-clear')
    send(34570, 'new-after-clear')
    run(generator, '--generate-rules', 'none', 'offline')
    send(34571, 'new-offline-remap')
    send(34571, 'new-offline-remap-second')
    time.sleep(.3)
    for p, f in captures:
        p.send_signal(signal.SIGINT)
        p.wait(timeout=5)
        f.close()
    text = {dev: run('tcpdump', '-Z', 'root', '-nn', '-r', str(root / (dev + '.pcap'))) for dev in ['wan1', 'wan2']}
    expected = {'34567': ('wan2', '10.2.0.1', 2), '34568': ('wan2', '10.2.0.1', 2), '34569': ('wan1', '10.1.0.1', 3), '34570': ('wan2', '10.2.0.1', 1), '34571': ('wan1', '10.1.0.1', 2)}
    for port, (dev, addr, count) in expected.items():
        other = 'wan1' if dev == 'wan2' else 'wan2'
        rows = [line for line in text[dev].splitlines() if addr + '.' + port + ' > 203.0.113.10.9000' in line]
        assert len(rows) == count, (port, 'missing correct path', rows)
        assert not any('.' + port + ' > 203.0.113.10.9000' in line for line in text[other].splitlines()), (port, 'wrong path')
        print('PASS flow ' + port + ' remains ' + dev + ' / SNAT ' + addr)
    trace.terminate()
    trace.wait(timeout=5)
    tracefile.close()
    # mark mask 以外的标志位在改写后也保留 (0x80004000)。
    trace_text = (root / 'trace.txt').read_text()
    assert 'meta mark 0x80004100' in trace_text, 'pin lost non-mwan mark bits'
    assert 'meta mark 0x80004200' in trace_text, 'existing flow lost non-mwan mark bits'
    print('PASS preserve non-mwan mark bits')
    print(run(generator, '--test-accounting').strip())
    run(generator, '--test-recovery')
    print('PASS established flow, unreplied confirmed flow, new pin, pin removal, offline remap, deleted-table recovery')
finally:
    for p in children:
        if p.poll() is None:
            p.terminate()
    for p in children:
        try:
            p.wait(timeout=2)
        except sp.TimeoutExpired:
            p.kill()
            p.wait()
