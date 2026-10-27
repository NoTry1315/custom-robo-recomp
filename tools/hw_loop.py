"""Drive the 3DS for hardware runs without anyone at the console: ftpd -> Homebrew Launcher -> game -> ftpd.
Inputs go through Rosalina's InputRedirection (UDP 4950), apps are started with the Homebrew Launcher's
netloader (3dslink, TCP 17491). The only state read is whether ftpd answers on TCP 5000.
Once per boot, by hand: Rosalina menu (L+Down+Select) -> Miscellaneous options -> Start InputRedirection;
start the Homebrew Launcher (or ftpd.3dsx from it). Keep the console plugged in with the lid open (sleep drops
Wi-Fi). Leave the Rosalina debugger off: twice the 3DS lost its network while this script polled the debugger's
process list (qXfer:osdata on TCP 4000, 2026-10-26).
Nothing may connect to TCP 17491 except 3dslink: the netloader takes any connection as an upload and shows an
error screen (B closes it). Its UDP ping answer does not reach this PC (firewall), so it is not used either.

    PYTHONUTF8=1 python tools/hw_loop.py state
    PYTHONUTF8=1 python tools/hw_loop.py press START [--hold 150]     (A B X Y L R ZL ZR START SELECT UP DOWN
                                                                       LEFT RIGHT HOME POWER; several = chord)
    PYTHONUTF8=1 python tools/hw_loop.py touch 160 120                (bottom screen, 320x240)
    PYTHONUTF8=1 python tools/hw_loop.py launch <file.3dsx>           (Homebrew Launcher -> Y -> 3dslink)
    PYTHONUTF8=1 python tools/hw_loop.py ftp ls|get|put|mv ...        (ftpd on the SD; no delete)
    PYTHONUTF8=1 python tools/hw_loop.py run --build custom-robo-recomp.3dsx --name 2026-10-26_hw-run37-x \\
        --wait 120 [--put local=remote ...] [--mv a=b ...] [--timeout 1800]
(Git Bash: MSYS_NO_PATHCONV=1, else SD paths like /3ds/.. are rewritten to Windows paths.)

`run`: in ftpd, uploads --put files and applies --mv renames (flag files on/off), START (ftpd exits to the
Homebrew Launcher), Y + 3dslink the build (the netloader writes sdmc:/3ds/<basename> and starts it). The run
length ends the game (runtime/vi.c) and it exits to the Homebrew Launcher. After --wait seconds (longer than the
run takes), Y + 3dslink ftpd.3dsx every 20 s until it connects; Y reaches the game if it is still running, so
make --wait generous. Then downloads /cr_autotest.log to logs/cr_autotest_<name>.log, renames it on the SD
to /cr_autotest_<name>.log and presses START: the 3DS is left in the Homebrew Launcher, as a run starts from
there or from ftpd. After any manual `ftp`/`launch` use, leave it there too (`press START` in ftpd)."""
import argparse, ftplib, os, socket, struct, subprocess, time

IP = os.environ.get('CR_3DS_IP', '192.168.1.129')
FTP_PORT, FTP_USER, FTP_PASS = 5000, 'a', 'a'
IR_PORT = 4950
IMAGE = 'mkw3ds-build:2026-10-09'
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FTPD_3DSX = os.path.join(REPO, 'tools', 'hw', 'ftpd.3dsx')   # mtheall/ftpd v3.2.1 release asset (not committed)

# InputRedirection packet (Luma3DS rosalina/source/input_redirection.c, TuxSH/InputRedirectionClient-Qt):
# 5 little-endian words: HID buttons (12 bits, pressed = 0), touch, circle pad, C-stick/ZL/ZR, HOME/POWER.
HID = {'A': 0, 'B': 1, 'SELECT': 2, 'START': 3, 'RIGHT': 4, 'LEFT': 5, 'UP': 6, 'DOWN': 7, 'R': 8, 'L': 9,
       'X': 10, 'Y': 11}
IRB = {'ZR': 1, 'ZL': 2}
SPECIAL = {'HOME': 0, 'POWER': 1, 'POWER_LONG': 2}
TOUCH_IDLE, CIRCLE_IDLE, CPP_IDLE = 0x2000000, 0x7ff7ff, 0x80800081


def packet(buttons=(), touch=None):
    hid, irb, spe = 0xfff, 0, 0
    for b in buttons:
        if b in HID: hid &= ~(1 << HID[b])
        elif b in IRB: irb |= 1 << IRB[b]
        elif b in SPECIAL: spe |= 1 << SPECIAL[b]
        else: raise SystemExit(f'unknown button {b}')
    t = TOUCH_IDLE
    if touch:
        x = 0xfff * min(max(0, touch[0]), 320) // 320
        y = 0xfff * min(max(0, touch[1]), 240) // 240
        t = (1 << 24) | (y << 12) | x
    return struct.pack('<5I', hid, t, CIRCLE_IDLE, CPP_IDLE | (irb << 8), spe)


def press(buttons=(), touch=None, hold_ms=150):
    """Holds the input for hold_ms, then sends the idle state (Rosalina keeps the last packet's values)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    down, idle = packet(buttons, touch), packet()
    end = time.time() + hold_ms / 1000
    while time.time() < end:
        s.sendto(down, (IP, IR_PORT)); time.sleep(0.02)
    for _ in range(5):
        s.sendto(idle, (IP, IR_PORT)); time.sleep(0.02)
    s.close()


def port_open(port, timeout=2.0):
    try:
        socket.create_connection((IP, port), timeout).close()
        return True
    except OSError:
        return False


def wait_for(what, cond, timeout, every=1.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond(): return True
        time.sleep(every)
    raise SystemExit(f'timed out after {timeout:.0f} s waiting for {what}')


def try_launch(path):
    """One attempt: Y (netloader; nothing if it is already open) + 3dslink. False if 3dslink could not connect."""
    d, f = os.path.split(os.path.abspath(path))
    press(['Y']); time.sleep(2)
    print(time.strftime('%H:%M:%S'), '3dslink', f, flush=True)
    cmd = ['docker', 'run', '--rm', '-v', f'{d}:/w', IMAGE, '/opt/devkitpro/tools/bin/3dslink', '-a', IP, f'/w/{f}']
    return subprocess.run(cmd, env=dict(os.environ, MSYS_NO_PATHCONV='1')).returncode == 0


def launch(path, timeout=60, every=10):
    end = time.time() + timeout
    while not try_launch(path):
        if time.time() > end: raise SystemExit(f'3dslink {path} did not connect for {timeout} s')
        time.sleep(every)
    time.sleep(3)


def ftp():
    f = ftplib.FTP()
    f.connect(IP, FTP_PORT, timeout=20)
    f.login(FTP_USER, FTP_PASS)
    return f


def to_ftpd(timeout=60, every=10):
    if port_open(FTP_PORT): return
    if not os.path.exists(FTPD_3DSX):
        raise SystemExit(f'{FTPD_3DSX} missing (ftpd.3dsx from github.com/mtheall/ftpd releases)')
    launch(FTPD_3DSX, timeout, every)
    wait_for('ftpd', lambda: port_open(FTP_PORT), 30)


def leave_ftpd():
    press(['START'])
    wait_for('ftpd to exit (START)', lambda: not port_open(FTP_PORT), 20)
    time.sleep(4)   # Homebrew Launcher reloads its menu (3dslink at 2 s was refused once)


def state():
    return {'ftpd': port_open(FTP_PORT)}


def run(a):
    log_local = os.path.join(REPO, 'logs', f'cr_autotest_{a.name}.log')
    if os.path.exists(log_local): raise SystemExit(f'{log_local} exists')
    to_ftpd()
    f = ftp()
    for kv in a.put:
        local, remote = kv.split('=', 1)
        with open(local, 'rb') as fh: f.storbinary(f'STOR {remote}', fh)
        print('put', local, '->', remote)
    for kv in a.mv:
        src, dst = kv.split('=', 1)
        f.rename(src, dst); print('mv', src, '->', dst)
    f.quit()
    leave_ftpd()
    launch(a.build)
    t0 = time.time()
    print(time.strftime('%H:%M:%S'), f'game started, waiting {a.wait:.0f} s', flush=True)
    time.sleep(a.wait)
    to_ftpd(a.timeout, 20)
    print(time.strftime('%H:%M:%S'), f'ftpd up {time.time() - t0:.0f} s after the game started', flush=True)
    f = ftp()
    os.makedirs(os.path.dirname(log_local), exist_ok=True)
    with open(log_local, 'wb') as fh: f.retrbinary('RETR /cr_autotest.log', fh.write)
    f.rename('/cr_autotest.log', f'/cr_autotest_{a.name}.log')
    f.quit()
    leave_ftpd()   # every run ends in the Homebrew Launcher, ready for the next one
    with open(log_local, 'rb') as fh:
        tail = fh.read()[-4000:].decode(errors='replace')
    done = [l for l in tail.splitlines() if l.startswith('# DONE')]
    print(os.path.relpath(log_local, REPO), os.path.getsize(log_local), 'bytes,', done[-1] if done else 'no # DONE line')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('state')
    p = sub.add_parser('press'); p.add_argument('buttons', nargs='+'); p.add_argument('--hold', type=int, default=150)
    p = sub.add_parser('touch'); p.add_argument('x', type=int); p.add_argument('y', type=int)
    p.add_argument('--hold', type=int, default=150)
    p = sub.add_parser('launch'); p.add_argument('file')
    p = sub.add_parser('ftp'); p.add_argument('op', choices=['ls', 'get', 'put', 'mv']); p.add_argument('args', nargs='*')
    p = sub.add_parser('run'); p.add_argument('--build', required=True); p.add_argument('--name', required=True)
    p.add_argument('--wait', type=float, required=True, help='seconds from game start to the first ftpd attempt')
    p.add_argument('--put', action='append', default=[]); p.add_argument('--mv', action='append', default=[])
    p.add_argument('--timeout', type=float, default=1800)
    a = ap.parse_args()
    if a.cmd == 'state': print(state())
    elif a.cmd == 'press': press([b.upper() for b in a.buttons], hold_ms=a.hold)
    elif a.cmd == 'touch': press(touch=(a.x, a.y), hold_ms=a.hold)
    elif a.cmd == 'launch': launch(a.file)
    elif a.cmd == 'run': run(a)
    elif a.cmd == 'ftp':
        f = ftp()
        if a.op == 'ls': print('\n'.join(f.nlst(a.args[0] if a.args else '/')))
        elif a.op == 'get':
            with open(a.args[1], 'wb') as fh: f.retrbinary(f'RETR {a.args[0]}', fh.write)
        elif a.op == 'put':
            with open(a.args[0], 'rb') as fh: f.storbinary(f'STOR {a.args[1]}', fh)
        elif a.op == 'mv': f.rename(a.args[0], a.args[1])
        f.quit()


if __name__ == '__main__':
    main()
