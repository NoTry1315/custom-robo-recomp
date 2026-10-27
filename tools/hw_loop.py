"""Hardware runs without anyone at the console, through cr_agent (tools/hw/agent) on the 3DS.
The agent is a small homebrew app with one TCP port (5001): it puts/gets/renames SD files and starts a .3dsx
through Luma3DS's 3dsx loader (hb:ldr, as the Homebrew Launcher does). A game build started with
"--return /3ds/cr_agent.3dsx" points the loader back at the agent when its run length ends (runtime/main.c
hw_return), so a run is: put the build and flag files, RUN, wait for the agent to answer again, get the log.
No button presses, no netloader, no ftpd, and nothing from Rosalina's network features.

Why: with Rosalina InputRedirection + debugger on (the old version of this script pressed buttons through
InputRedirection and started apps with 3dslink + ftpd), the 3DS lost Wi-Fi and froze 4 times in an hour on
2026-10-26, once with only ftpd idle; before InputRedirection was first turned on, 8 hours of hardware runs had no
network trouble. With the agent, 10 runs had none, but at 2026-10-27 01:57 the 3DS left the network with only the agent
running (a GET stopped at 64 KB after 3.4 h idle): not the whole cause, still open.
Both Rosalina threads poll the network every 10-50 ms from the system core (README "Unattended hardware runs").

Once per boot, by hand: Rosalina menu -> InputRedirection and debugger OFF (or simply reboot: both start off);
start cr_agent from the Homebrew Launcher. Keep the console plugged in with the lid open (sleep drops Wi-Fi).
First install (or an agent update): `agent-install` (needs a running agent), or put tools/hw/agent/cr_agent.3dsx
in sdmc:/3ds/ with any FTP client.

    PYTHONUTF8=1 python tools/hw_loop.py ping
    PYTHONUTF8=1 python tools/hw_loop.py ls|get|put|mv ...           (SD files; no delete, /custom-robo/save refused)
    PYTHONUTF8=1 python tools/hw_loop.py run --build custom-robo-recomp.3dsx --name 2026-10-26_hw-run37-x \\
        [--timeout 400] [--put local=remote ...] [--mv a=b ...]
    PYTHONUTF8=1 python tools/hw_loop.py quit                         (agent -> Homebrew Launcher)
    PYTHONUTF8=1 python tools/hw_loop.py agent-install                (put the built agent, RUN it: restarts it)
(Git Bash: MSYS_NO_PATHCONV=1, else SD paths like /3ds/.. are rewritten to Windows paths.)

`run`: --put uploads and --mv renames (flag files on/off), then the build goes to sdmc:/3ds/<basename> and is
started. The run needs a run length (pad script or replay, runtime/vi.c) so the game exits by itself. The script
does not connect to the console while the game runs: it waits --first s (default 120), then tries the agent port
every 15 s until --timeout s (default 600) from the start. Then it downloads /cr_autotest.log to
logs/cr_autotest_<name>.log and renames it on the SD to /cr_autotest_<name>.log. The 3DS is left in the agent,
ready for the next run; `quit` at the end of a session returns it to the Homebrew Launcher. A game that hangs or
crashes never returns to the agent: the script stops after --timeout and says so (look at the console).
Every agent PING reply (uptime, Wi-Fi strength 0-3, SD free) is appended to logs/hw_loop_agent.log."""
import argparse, os, socket, sys, time

IP = os.environ.get('CR_3DS_IP', '192.168.1.129')
AGENT_PORT = 5001
AGENT_SD = '/3ds/cr_agent.3dsx'
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AGENT_3DSX = os.path.join(REPO, 'tools', 'hw', 'agent', 'cr_agent.3dsx')
AGENT_LOG = os.path.join(REPO, 'logs', 'hw_loop_agent.log')
IR_PORT = 4950


def now():
    return time.strftime('%H:%M:%S')


class Agent:
    """One connection to cr_agent; line commands, see tools/hw/agent/source/main.c."""
    def __init__(self, timeout=20):
        self.s = socket.create_connection((IP, AGENT_PORT), timeout)
        self.s.settimeout(60)
        self.f = self.s.makefile('rb')

    def line(self):
        l = self.f.readline()
        if not l: raise OSError('agent closed the connection')
        return l.decode('utf-8', 'replace').rstrip('\r\n')

    def cmd(self, text):
        self.s.sendall(text.encode() + b'\n')
        r = self.line()
        if not r.startswith(('OK', 'READY')): raise SystemExit(f'agent: {text!r} -> {r}')
        return r

    def ping(self):
        r = self.cmd('PING')
        os.makedirs(os.path.dirname(AGENT_LOG), exist_ok=True)
        with open(AGENT_LOG, 'a') as fh: fh.write(f'{time.strftime("%Y-%m-%d %H:%M:%S")} {r}\n')
        return r

    def put(self, local, remote):
        data = open(local, 'rb').read()
        self.cmd(f'PUT {len(data)} {remote}')
        self.s.sendall(data)
        r = self.line()
        if r != f'OK {len(data)}': raise SystemExit(f'agent: PUT {remote} -> {r}')

    def get(self, remote, local):
        n = int(self.cmd(f'GET {remote}').split()[1])
        buf = bytearray()
        while len(buf) < n:
            b = self.f.read(min(65536, n - len(buf)))
            if not b: raise OSError(f'GET {remote}: connection closed at {len(buf)} of {n} bytes')
            buf += b
        with open(local, 'wb') as fh: fh.write(buf)
        return n

    def ls(self, path):
        self.cmd(f'LS {path}')
        out = []
        while (l := self.line()) != '.': out.append(l)
        return out

    def close(self):
        try: self.s.close()
        except OSError: pass


def agent(timeout=20):
    try:
        return Agent(timeout)
    except OSError as e:
        raise SystemExit(f'cr_agent not reachable at {IP}:{AGENT_PORT} ({e}). Start it from the Homebrew Launcher '
                         '(Rosalina InputRedirection and debugger off), then try again.')


def wait_agent(first, timeout, every=15):
    """Sleeps `first` s, then tries the agent port every `every` s; False after `timeout` s."""
    t0 = time.time()
    time.sleep(first)
    while time.time() - t0 < timeout:
        try:
            a = Agent(5)
            return a
        except OSError:
            time.sleep(every)
    return None


def run(a):
    log_local = os.path.join(REPO, 'logs', f'cr_autotest_{a.name}.log')
    if os.path.exists(log_local): raise SystemExit(f'{log_local} exists')
    if not os.path.exists(a.build): raise SystemExit(f'{a.build} missing')
    g = agent()
    print(now(), g.ping(), flush=True)
    for kv in a.put:
        local, remote = kv.split('=', 1)
        g.put(local, remote); print('put', local, '->', remote)
    for kv in a.mv:
        src, dst = kv.split('=', 1)
        g.cmd(f'MV {src} {dst}'); print('mv', src, '->', dst)
    remote = '/3ds/' + os.path.basename(a.build)
    g.put(a.build, remote)
    print(now(), 'put', a.build, '->', remote, flush=True)
    g.cmd(f'RUN {remote} --return {AGENT_SD}')
    g.close()
    t0 = time.time()
    print(now(), f'game started, first look after {a.first:.0f} s', flush=True)
    g = wait_agent(a.first, a.timeout)
    if not g:
        raise SystemExit(f'{now()} the agent did not come back within {a.timeout:.0f} s: the game hung or crashed, '
                         'or the network is down. Look at the console before anything else.')
    print(now(), f'agent back {time.time() - t0:.0f} s after the game started;', g.ping(), flush=True)
    os.makedirs(os.path.dirname(log_local), exist_ok=True)
    # run 39a (2026-10-26): a GET 4 s after the agent restarted stalled for 60 s; the same GET worked a minute
    # later. Give the freshly started agent's Wi-Fi a moment, and retry a failed download once.
    g.close(); time.sleep(10)
    for i in range(2):
        try:
            g = agent(); n = g.get('/cr_autotest.log', log_local); break
        except OSError as e:
            if i: raise
            print(now(), f'log download failed ({e}), retrying in 20 s', flush=True)
            try: g.close()
            except OSError: pass
            time.sleep(20)
    g.cmd(f'MV /cr_autotest.log /cr_autotest_{a.name}.log')
    g.close()
    with open(log_local, 'rb') as fh:
        tail = fh.read()[-4000:].decode(errors='replace')
    done = [l for l in tail.splitlines() if l.startswith('# DONE')]
    print(os.path.relpath(log_local, REPO), n, 'bytes,', done[-1] if done else 'no # DONE line', flush=True)
    if not done: sys.exit(1)


# InputRedirection (Rosalina, UDP 4950): not used by `run`. Kept for a one-off press with someone at the
# console; turn InputRedirection off again afterwards (suspected in the 2026-10-26 Wi-Fi crashes).
HID = {'A': 0, 'B': 1, 'SELECT': 2, 'START': 3, 'RIGHT': 4, 'LEFT': 5, 'UP': 6, 'DOWN': 7, 'R': 8, 'L': 9,
       'X': 10, 'Y': 11}


def press(buttons, hold_ms=150):
    import struct
    hid = 0xfff
    for b in buttons:
        if b not in HID: raise SystemExit(f'unknown button {b}')
        hid &= ~(1 << HID[b])
    down = struct.pack('<5I', hid, 0x2000000, 0x7ff7ff, 0x80800081, 0)
    idle = struct.pack('<5I', 0xfff, 0x2000000, 0x7ff7ff, 0x80800081, 0)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    end = time.time() + hold_ms / 1000
    while time.time() < end:
        s.sendto(down, (IP, IR_PORT)); time.sleep(0.02)
    for _ in range(5):
        s.sendto(idle, (IP, IR_PORT)); time.sleep(0.02)
    s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('ping'); sub.add_parser('quit'); sub.add_parser('agent-install')
    p = sub.add_parser('ls'); p.add_argument('path', nargs='?', default='/')
    p = sub.add_parser('get'); p.add_argument('remote'); p.add_argument('local')
    p = sub.add_parser('put'); p.add_argument('local'); p.add_argument('remote')
    p = sub.add_parser('mv'); p.add_argument('src'); p.add_argument('dst')
    p = sub.add_parser('press'); p.add_argument('buttons', nargs='+'); p.add_argument('--hold', type=int, default=150)
    p = sub.add_parser('run'); p.add_argument('--build', required=True); p.add_argument('--name', required=True)
    p.add_argument('--first', type=float, default=120, help='seconds before the first look for the agent')
    p.add_argument('--timeout', type=float, default=600, help='seconds from the game start to give up')
    p.add_argument('--put', action='append', default=[]); p.add_argument('--mv', action='append', default=[])
    a = ap.parse_args()
    if a.cmd == 'run': return run(a)
    if a.cmd == 'press': return press([b.upper() for b in a.buttons], a.hold)
    g = agent()
    if a.cmd == 'ping': print(g.ping())
    elif a.cmd == 'ls': print('\n'.join(g.ls(a.path)))
    elif a.cmd == 'get': print(g.get(a.remote, a.local), 'bytes')
    elif a.cmd == 'put': g.put(a.local, a.remote)
    elif a.cmd == 'mv': g.cmd(f'MV {a.src} {a.dst}')
    elif a.cmd == 'quit': g.cmd('QUIT')
    elif a.cmd == 'agent-install':
        g.put(AGENT_3DSX, AGENT_SD); g.cmd(f'RUN {AGENT_SD}'); print('agent replaced and restarted')
    g.close()


if __name__ == '__main__':
    main()
