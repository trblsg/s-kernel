#!/usr/bin/env python3
"""
fps.py RUN.log [MORE.log ...]: how many frames a run with boot-arg fps_log=1 showed, and what held them.
(More logs: the live ones of the same run, in order; pass one log per run to compare two runs.)

Reads the "fps:" lines the kernel writes once a second while frames come (boot-arg fps_log=1,
MiniIOMobileFramebuffer.cpp; see configs/symbols.tsv). A second that showed a frame has three lines:

  fps: N presented, N drawn, N flips, N lost, N held; drawing US a frame, US at most; waiting for
       the flip before US, ...                 the swaps handed over, the frames drawn, the panel's
                                                flips, lost and held frames; the render thread's time
                                                drawing a frame and waiting for the previous flip
  fps: N refreshes missed ...                   refreshes missed while frames came and the longest
                                                gap, fb_zc's frames, the GPU's busy time, a frame
                                                shown, its jobs, clock and the G3D's temperature
  fps: cpu: ...                                 the render thread's processor time a frame, on which
                                                processors it handed the swaps over

A second with no frames leaves one "fps: idle (...)" line (the first idle second only).

The frames a second are "presented": the swaps handed to the display, the closest the kernel has to
what a program drew. A second with fewer than 10 of them is counted as still and left out of the
moving numbers (a screen that updates a clock, a pause between gestures); FPS_MOVING_FROM=N changes
the threshold. Times are the means the kernel printed, weighted by the frames they cover; p90 and
the most are over the seconds, so one slow second shows in them.

The lines also appear in the loader's log, and the log of a run that came back through the kernel
can carry them twice: a second whose three lines repeat the second before's exactly is counted once.
"""
import os
import re
import sys

MOVING_FROM = int(os.environ.get('FPS_MOVING_FROM', '10'))
TOP = 8

PRESENTED = re.compile(
    r'fps: (\d+) presented, (\d+) drawn, (\d+) flips, (\d+) lost, (\d+) held; '
    r'drawing (\d+) us a frame, (\d+) at most; '
    r'waiting for the flip before (\d+) us, (\d+) at most; '
    r'(\d+) refreshes; (\d+) partial; (-?\d+) refresh notifications sent; '
    r'(\d+) from the surface before, (\d+) not done by the GPU; (.*)$')
MISSED = re.compile(
    r'fps: (\d+) refreshes missed while moving, the longest gap (\d+)\.(\d+) ms; '
    r'zc: (\d+) in place, (\d+) copied, (\d+) into the one shown; '
    r'gpu: (none; )?(\d+)% busy, (\d+) us a frame shown, (\d+) jobs '
    r'\((\d+) us each, (\d+) us queued\), (\d+) MHz(?: \(mali_gov\))?, '
    r'(\d+) switches, G3D (-?\d+) C$')
CPU = re.compile(
    r'fps: cpu: the render thread (\d+) us a frame, (\d+) at most, (\d+) frames, '
    r'on cpus (0x[0-9a-fA-F]+)(?: \(fb_rt\))?; gpu (\d+) us a frame; '
    r'zc: (\d+) surfaces in turn, (\d+) drawn into the one shown not copied; '
    r'fb_zc_hold (\d+); swapwait: (\d+) \((\d+) for the last swap, '
    r'(\d+) the one before, (\d+) other\), (\d+) waited (\d+) us each, '
    r'(\d+) for 2 refreshes(?: \(fb_swapwait\))?$')
IDLE = re.compile(r'fps: idle \((\d+) refreshes in the last second; (.*)\)$')


def lines_of(paths):
    """The fps: lines, in order, from wherever they start in a line (other writers interleave)."""
    for path in paths:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace')
                at = text.find('fps: ')
                if at >= 0:
                    yield text[at:].strip()


def parse(paths):
    samples, idle, other, incomplete = [], 0, 0, 0
    cur = None

    def finish():
        nonlocal cur, incomplete
        if cur is None:
            return
        if cur.get('presented') is None:
            incomplete += 1
        elif samples and samples[-1].get('sig') == cur.get('sig'):
            pass                                    # the same three lines again (the loader's copy)
        else:
            samples.append(cur)
        cur = None

    for line in lines_of(paths):
        m = PRESENTED.match(line)
        if m:
            finish()
            cur = dict(sig=[line], presented=int(m[1]), drawn=int(m[2]), flips=int(m[3]),
                       lost=int(m[4]), held=int(m[5]), draw_us=int(m[6]), draw_max_us=int(m[7]),
                       wait_us=int(m[8]), wait_max_us=int(m[9]), refreshes=int(m[10]),
                       partial=int(m[11]), notifications=int(m[12]), same=int(m[13]),
                       undrawn=int(m[14]), mode=m[15])
            continue
        m = MISSED.match(line)
        if m:
            if cur is None:
                cur = dict(sig=[], presented=None)
            cur['sig'].append(line)
            cur.update(missed=int(m[1]), gap_ms=float('%s.%s' % (m[2], m[3])),
                       zc=int(m[4]), zc_copied=int(m[5]), zc_busy=int(m[6]),
                       gpu_present=m[7] is None, busy=int(m[8]), gpu_us=int(m[9]),
                       jobs=int(m[10]), job_us=int(m[11]), queued_us=int(m[12]),
                       mhz=int(m[13]), switches=int(m[14]), g3d=int(m[15]))
            continue
        m = CPU.match(line)
        if m:
            if cur is None:
                cur = dict(sig=[], presented=None)
            cur['sig'].append(line)
            cur.update(cpu_us=int(m[1]), cpu_max_us=int(m[2]), cpu_frames=int(m[3]),
                       cpus=m[4], gpu_cpu_us=int(m[5]), zc_surfaces=int(m[6]),
                       zc_again=int(m[7]), zc_hold=int(m[8]), waits=int(m[9]),
                       wait_last=int(m[10]), wait_before=int(m[11]), wait_other=int(m[12]),
                       waits_slept=int(m[13]), wait_slept_us=int(m[14]),
                       waits_timeout=int(m[15]))
            finish()
            continue
        if IDLE.match(line):
            finish()
            idle += 1
            continue
        other += 1
    finish()
    return samples, idle, other, incomplete


def pct(values, p):
    """The nearest value at p percent (p10 is the slow tail, p90 the slow times' tail)."""
    values = sorted(values)
    if not values:
        return 0
    return values[max(0, min(len(values) - 1, int(round(p / 100 * (len(values) - 1)))))]


def mean(values):
    return sum(values) / len(values) if values else 0


def weighted(samples, per_frame, frames):
    den = sum(s.get(frames, 0) for s in samples)
    return sum(s.get(per_frame, 0) * s.get(frames, 0) for s in samples) / den if den else 0


def ms(us):
    """Microseconds as milliseconds: two decimals below 1 ms (drawing is often tens of us)."""
    value = us / 1000
    return '%.2f' % value if value < 1 else '%.1f' % value


def main():
    paths, top, show_all = [], TOP, False
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a in ('-h', '--help'):
            print(__doc__)
            return
        if a == '--all':
            show_all = True
        elif a.startswith('--top='):
            top = int(a.split('=', 1)[1])
        elif a == '--top':
            i += 1
            top = int(args[i])
        else:
            paths.append(a)
        i += 1
    if not paths:
        sys.exit(__doc__)
    top = max(0, top)

    samples, idle, other, incomplete = parse(paths)
    active = [s for s in samples if s.get('presented', 0) > 0 or s.get('drawn', 0) > 0]
    moving = [s for s in active if s['presented'] >= MOVING_FROM]
    going = moving or active
    if not active:
        print('fps.py: no fps: lines with frames: was the run booted with fps_log=1?')
        return

    print('fps.py: %d seconds with frames (%d moving, %d still), %d idle, %d other fps: lines%s'
          % (len(active), len(moving), len(active) - len(moving), idle, other,
             '' if not incomplete else ', %d with a line missing' % incomplete))

    fps = [s['presented'] for s in going]
    refresh = pct([s.get('refreshes', 0) for s in going if s.get('refreshes', 0) > 0], 50)
    high = max(1, refresh - 5) if refresh else 55
    low = max(1, refresh // 2) if refresh else 30
    at_high = 100 * sum(1 for f in fps if f >= high) / len(fps)
    under = 100 * sum(1 for f in fps if f < low) / len(fps)
    print('%s: %.1f presented on average, %d at the median, %d at p10, %d at least; '
          '%.0f%% of the %s seconds were %d or more, %.0f%% under %d%s'
          % ('a moving second' if moving else 'a second',
             mean(fps), pct(fps, 50), pct(fps, 10), min(fps), at_high,
             'moving' if moving else 'with frames', high, under, low,
             ' (%d refreshes a second)' % refresh if refresh else ''))

    drawn = sum(s.get('drawn', 0) for s in active)
    print('frames: %d presented, %d drawn, %d flips; %d lost, %d held; %d partial'
          % (sum(s['presented'] for s in active), drawn, sum(s.get('flips', 0) for s in active),
             sum(s.get('lost', 0) for s in active), sum(s.get('held', 0) for s in active),
             sum(s.get('partial', 0) for s in active)))

    if going:
        draw = weighted(going, 'draw_us', 'drawn')
        wait = weighted(going, 'wait_us', 'drawn')
        print('drawing: %s ms a frame (%s at p90, %s at most); waiting for the flip: '
              '%s ms (%s at p90, %s at most)'
              % (ms(draw), ms(pct([s.get('draw_us', 0) for s in going], 90)),
                 ms(max(s.get('draw_max_us', 0) for s in going)), ms(wait),
                 ms(pct([s.get('wait_us', 0) for s in going], 90)),
                 ms(max(s.get('wait_max_us', 0) for s in going))))

        gpu = [s for s in going if s.get('gpu_present')]
        if gpu:
            print('gpu: %.0f%% busy (%.0f at p90), %s ms a frame shown (%s at p90); '
                  '%d refreshes missed while moving, the longest gap %.1f ms'
                  % (mean([s['busy'] for s in gpu]), pct([s['busy'] for s in gpu], 90),
                     ms(weighted(gpu, 'gpu_us', 'drawn')),
                     ms(pct([s['gpu_us'] for s in gpu], 90)),
                     sum(s.get('missed', 0) for s in active),
                     max(s.get('gap_ms', 0) for s in active)))
        else:
            print('gpu: no device in this run (the lines say "none")')

        counted = [s for s in going if s.get('cpu_frames', 0) > 0]
        if counted:
            print('render thread: %s ms a frame (%s at p90, %s at most), %.1f frames a second'
                  % (ms(weighted(counted, 'cpu_us', 'cpu_frames')),
                     ms(pct([s['cpu_us'] for s in counted], 90)),
                     ms(max(s.get('cpu_max_us', 0) for s in counted)),
                     sum(s['cpu_frames'] for s in counted) / len(going)))

        clock = [s for s in gpu if s.get('mhz', 0) > 0]
        if clock:
            line = ('clock: %d MHz at the median (%d least, %d most), %d switches'
                    % (pct([s['mhz'] for s in clock], 50), min(s['mhz'] for s in clock),
                       max(s['mhz'] for s in clock), sum(s.get('switches', 0) for s in clock)))
            warm = [s for s in clock if s.get('g3d', 0) > 0]
            if warm:
                line += ('; G3D %d C at the median (%d least, %d most)'
                         % (pct([s['g3d'] for s in warm], 50), min(s['g3d'] for s in warm),
                            max(s['g3d'] for s in warm)))
            print(line)

        print('zero-copy: %d frames in place, %d copied, %d into the one shown'
              % (sum(s.get('zc', 0) for s in active), sum(s.get('zc_copied', 0) for s in active),
                 sum(s.get('zc_busy', 0) for s in active)))

    def second(n, s):
        line = ('  #%3d: %3d presented; drawing %s ms, waiting %s ms, gpu %s ms; '
                '%d lost, %d held' % (n, s['presented'], ms(s.get('draw_us', 0)),
                                      ms(s.get('wait_us', 0)), ms(s.get('gpu_us', 0)),
                                      s.get('lost', 0), s.get('held', 0)))
        if s.get('mhz', 0) > 0:
            line += '; %d MHz' % s['mhz']
        if s.get('g3d', 0) > 0:
            line += '; G3D %d C' % s['g3d']
        return line

    numbered = list(enumerate(active, 1))
    if show_all:
        chosen = numbered
        print('every second with frames:')
    else:
        pool = ([ns for ns in numbered if ns[1]['presented'] >= MOVING_FROM] if moving
                else numbered)
        chosen = sorted(pool, key=lambda ns: (ns[1]['presented'], -ns[1].get('gpu_us', 0)))[:top]
        if not chosen:
            return
        print('the %d slowest %s seconds:' % (len(chosen), 'moving' if moving else 'with frames'))
    for n, s in chosen:
        print(second(n, s))


if __name__ == '__main__':
    main()
