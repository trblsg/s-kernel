#!/usr/bin/env python3
"""uirun.py : drive the phone's UI from the Mac over the kernel's USB console, with nobody at the phone.

  uirun.py [--phone 1|2 | --port /dev/cu.usbmodemN] [--shots DIR] [--log FILE] SCRIPT      run a script file
  uirun.py [--phone ...] -c 'home; wait 2; tap 500 500; shot settings'                     or steps on the command line

Needs a kernel with the console's touch and button verbs (the fork's wip/ios27-console-touch) and usb_gadget=5. With two
phones on one Mac the node is found by USB location (--phone; the LOC table below is one bench's), because both phones'
gadgets carry the same serial string; --port names the node directly.

Script: one step a line ('#' starts a comment; ';' separates steps on one line). Coordinates are THOUSANDTHS of the
screen: 0 0 top left, 1000 1000 bottom right (so they do not depend on the canvas or the panel).
  tap X Y                     a tap
  swipe X1 Y1 X2 Y2 [MS]      a swipe (default 300 ms)
  home | lock | volup | voldown [MS]     a button (80 ms; e.g. 'lock 1500' is a long press)
  wait S                      seconds (decimals allowed)
  shot [NAME]                 a picture of the panel (console verb "shot 2": every 2nd pixel, 720x1480) -> DIR/NN-NAME.png
  raw TEXT                    any console line as it is (e.g. 'raw sysctl kern.hakios.brightness=200', 'raw uptime')
  return                      end the run: the loader restarts the phone into TWRP
Common gestures (iPhone-X style UI on the S9's canvas):
  unlock: home; wait 1; home          Control Center: swipe 930 3 930 500          app switcher: swipe 500 995 500 600 600
  go home from an app: home           scroll down a list: swipe 500 750 500 350    next home page: swipe 880 500 120 500

Who reads the port: the console is ONE stream. If a log reader already has the node open (a `cat` of the node into a
file, say), this tool only WRITES the verbs, and the pictures are cut out of that reader's file (the stream holds them framed:
"SHOT w h stride rgb888 bytes\\n<pixels>END\\n"): pass --log FILE (the reader's file) and they are extracted as they
arrive. With no reader on the node and no --log, this tool reads the port itself for the pictures (and prints the text).
Every step is logged with its time to DIR/steps.log.
"""
import argparse, os, re, struct, subprocess, sys, time, zlib

LOC = {'1': 1048576, '2': 17825792}

def node_for(loc):
    out = subprocess.run(['ioreg', '-r', '-c', 'IOUSBHostDevice', '-l', '-w0'], capture_output=True, text=True).stdout
    cur = None
    for ln in out.splitlines():
        m = re.search(r'"locationID" = (\d+)', ln)
        if m: cur = int(m.group(1))
        m = re.search(r'"IOCalloutDevice" = "(/dev/cu\.usbmodem[^"]*)"', ln)
        if m and cur == loc: return m.group(1)
    return None

def png(path, w, h, stride, rgb):
    rows = b''.join(b'\0' + rgb[y * stride: y * stride + w * 3] for y in range(h))
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))

HDR = re.compile(rb'SHOT (\d+) (\d+) (\d+) rgb888 (\d+)\n')

def cut_shot(data, start):
    """the first whole framed picture at or after `start` of `data`: (w, h, stride, pixels, end) or None"""
    m = HDR.search(data, start)
    if not m: return None
    w, h, stride, n = (int(x) for x in m.groups())
    if len(data) < m.end() + n: return None
    return w, h, stride, data[m.end(): m.end() + n], m.end() + n

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--phone', default='2', choices=['1', '2'])
    ap.add_argument('--port')
    ap.add_argument('--shots', default='shots-' + time.strftime('%m%d-%H%M%S'))
    ap.add_argument('--log', help="the file a log reader appends the console to (pictures are cut out of it)")
    ap.add_argument('-c', dest='inline')
    ap.add_argument('script', nargs='?')
    a = ap.parse_args()
    text = a.inline if a.inline else open(a.script).read()
    steps = [s.strip() for ln in text.splitlines() for s in ln.split('#')[0].split(';') if s.strip()]
    port = a.port or node_for(LOC[a.phone])
    if not port: sys.exit('uirun: phone %s has no console node (not booted into iOS, or no usb_gadget=5)' % a.phone)
    os.makedirs(a.shots, exist_ok=True)
    slog = open(os.path.join(a.shots, 'steps.log'), 'a')
    def say(s):
        line = '%.2f %s' % (time.time(), s); print(line, flush=True); slog.write(line + '\n'); slog.flush()
    say('port %s, %d steps, pictures -> %s%s' % (port, len(steps), a.shots, ', cut from ' + a.log if a.log else ''))
    reader = None
    if not a.log:
        # nobody else reads: take the port ourselves (raw, no echo), for the pictures
        import termios, tty
        reader = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(reader)
        t = termios.tcgetattr(reader); t[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY); t[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)
        termios.tcsetattr(reader, termios.TCSANOW, t)
    def send(line):
        if reader is not None: os.write(reader, (line + '\n').encode())
        else:
            with open(port, 'wb', buffering=0) as f: f.write((line + '\n').encode())
    shot_no = 0
    for step in steps:
        w = step.split()
        verb, args = w[0].lower(), w[1:]
        if verb == 'wait':
            say('wait ' + args[0]); time.sleep(float(args[0])); continue
        if verb == 'tap':
            send('touch %s %s' % (args[0], args[1])); say(step); time.sleep(0.35)
        elif verb == 'swipe':
            ms = int(args[4]) if len(args) > 4 else 300
            send('touch %s %s %s %s %d' % (args[0], args[1], args[2], args[3], ms)); say(step); time.sleep(ms / 1000 + 0.35)
        elif verb in ('home', 'lock', 'volup', 'voldown'):
            name = {'volup': 'up', 'voldown': 'down'}.get(verb, verb)
            ms = int(args[0]) if args else 80
            send('button %s %d' % (name, ms)); say(step); time.sleep(ms / 1000 + 0.3)
        elif verb == 'raw':
            send(' '.join(args)); say(step); time.sleep(0.3)
        elif verb == 'return':
            send('return'); say('return (the phone restarts into TWRP)'); break
        elif verb == 'shot':
            shot_no += 1
            name = '%02d-%s.png' % (shot_no, args[0] if args else 'shot')
            path = os.path.join(a.shots, name)
            if a.log:
                at = os.path.getsize(a.log) if os.path.exists(a.log) else 0
                send('shot 2'); got = None; t0 = time.time()
                while time.time() - t0 < 25 and not got:
                    time.sleep(0.5)
                    with open(a.log, 'rb') as f:
                        f.seek(at); got = cut_shot(f.read(), 0)
            else:
                buf = b''; send('shot 2'); got = None; t0 = time.time()
                while time.time() - t0 < 25 and not got:
                    try: buf += os.read(reader, 1 << 16)
                    except BlockingIOError: time.sleep(0.02)
                    got = cut_shot(buf, 0)
            if got:
                png(path, got[0], got[1], got[2], got[3]); say('shot -> %s (%dx%d)' % (path, got[0], got[1]))
            else:
                say('shot %s FAILED: no framed picture within 25 s (does this kernel answer "shot"? is --log the right file?)' % name)
        else:
            say('unknown step: ' + step)
    say('done')

if __name__ == '__main__':
    main()
