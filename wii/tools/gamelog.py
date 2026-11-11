#!/usr/bin/env python3
"""Reads the Diagnostics setting's records of online games
(sd:/apps/melee/logs/diag/game-*.bin, written by wii/src/slippi/so_gamelog.c).

    gamelog.py FILE...            each game's text and a summary
    gamelog.py --csv FILE         its polls, a line each
    gamelog.py --packets FILE     the opponent's pad packets, a line each
    gamelog.py --slow FILE        the frames that took over 21 ms, with what happened in them
    gamelog.py --stalls FILE      each time the opponent's packets stopped for over 100 ms, with what
                                  the console did beside the game just before and during it
    gamelog.py --events FILE      what the console did beside the game, a line each
    gamelog.py --signal FILE      the Wi-Fi signal (every 250 ms), a line each

The Wi-Fi signal is the last frame the Wii's Wi-Fi module received, as IOS
keeps it (0x800031A0; the SDK's WDGetRadioLevel). Read as a signed byte it is
dBm, by the SDK's levels: 3 from -60, 2 from -75, 1 from -85, else 0.

With config.ini's delay_adjust=1, the summary lists each change of the
input delay during the game (when, and the new delay).

A poll is the game asking for a frame's inputs: once a frame, more when it
waits. A poll's line holds what happened until the next one.
"""
import argparse
import struct
import sys

POLL = struct.Struct('>IHBbBBBBBBBb')
PACKET = struct.Struct('>IHBB')
EVENT = struct.Struct('>IBBH')
EVENTS = {'B': 'broadcast', 'D': 'disc read', 'R': 'replay write', 'L': 'log line', 'T': 'slow send',
          'V': 'slow receive'}
ANSWERS = ['play', 'wait:inputs', 'wait:sync', 'wait:pad', 'other']
POLL_FIELDS = ('t_us frame answer ahead rolled packets retraces saves save_us load_us wait_us ping_100us '
               'offset_100us answer_us').split()


def unwrap(low, last):
    """A frame from its low 16 bits and the frame before it."""
    frame = (last & ~0xFFFF) | low
    if frame < last - 0x8000:
        frame += 0x10000
    return frame


def read(path):
    data = open(path, 'rb').read()
    if data[:8] not in (b'MWDIAG2\0', b'MWDIAG3\0'):
        sys.exit(f'{path}: not a diagnostics record (or one of another version)')
    text_len, poll_size, poll_count, packet_size, packet_count = struct.unpack('>5I', data[8:28])
    at = 28
    event_size = event_count = 0
    if data[:8] == b'MWDIAG3\0':
        event_size, event_count = struct.unpack('>2I', data[28:36])
        at = 36
    text = data[at:at + text_len].decode('latin1')
    at += text_len
    polls = []
    frame = 0
    for i in range(poll_count):
        rec = data[at + i * poll_size:at + (i + 1) * poll_size]
        if len(rec) < POLL.size:
            break
        t, low, answer, ahead, rolled, packets, save, load, wait, took, ping, offset = POLL.unpack(rec[:POLL.size])
        frame = unwrap(low, frame)
        polls.append({'t_us': t, 'frame': frame, 'answer': answer & 7, 'retraces': answer >> 3, 'ahead': ahead,
                      'rolled': rolled & 15, 'saves': rolled >> 4, 'packets': packets, 'save_us': save * 100,
                      'load_us': load * 100, 'wait_us': wait * 100, 'answer_us': took * 100, 'ping_100us': ping * 10,
                      'offset_100us': offset * 10})
    at += poll_count * poll_size
    packets = []
    frame = 0
    for i in range(packet_count):
        rec = data[at + i * packet_size:at + (i + 1) * packet_size]
        if len(rec) < PACKET.size:
            break
        t, low, added, ping = PACKET.unpack(rec[:PACKET.size])
        frame = unwrap(low, max(frame - 0x4000, 0))
        packets.append({'t_us': t, 'frame': frame, 'added': added & 63, 'remote': added >> 6, 'ping_100us': ping * 10})
    for i, p in enumerate(polls):
        p['gap_us'] = polls[i + 1]['t_us'] - p['t_us'] if i + 1 < len(polls) else 0
    at += packet_count * packet_size
    events = []
    signals = []
    delays = []
    for i in range(event_count):
        rec = data[at + i * event_size:at + (i + 1) * event_size]
        if len(rec) < EVENT.size:
            break
        t, code, value, took = EVENT.unpack(rec[:EVENT.size])
        if chr(code) == 'W':
            signals.append({'t_us': t, 'raw': value, 'dbm': value - 256 if value > 127 else value})
        elif chr(code) == 'A':
            delays.append({'t_us': t, 'delay': value})
        else:
            events.append({'t_us': t, 'code': chr(code), 'took_us': took * 100})
    events.sort(key=lambda e: e['t_us'])
    signals.sort(key=lambda e: e['t_us'])
    read.events = events
    read.signals = signals
    read.delays = sorted(delays, key=lambda e: e['t_us'])
    return text, polls, packets


def level(dbm):
    return 3 if dbm >= -60 else 2 if dbm >= -75 else 1 if dbm >= -85 else 0


def signal_near(signals, t_from, t_to):
    """The Wi-Fi signal readings from t_from to t_to, as text."""
    near = [s['dbm'] for s in signals if t_from <= s['t_us'] <= t_to]
    return ' '.join(str(x) for x in near) if near else '-'


def stalls(polls, packets, events, signals=()):
    """Each time the opponent's packets stopped for over 100 ms."""
    print('from_s to_s gap_ms | ping_ms_after frames_ahead | Wi-Fi dBm from 1 s before to its end | beside the game, '
          'from 300 ms before to its end (ms from its start: what, ms it took)')
    for a, b in zip(packets, packets[1:]):
        gap = b['t_us'] - a['t_us']
        if gap <= 100000:
            continue
        near = [p for p in polls if a['t_us'] <= p['t_us'] <= b['t_us'] + 100000]
        around = [e for e in events if a['t_us'] - 300000 <= e['t_us'] <= b['t_us']]
        print(f'{a["t_us"] / 1e6:.2f} {b["t_us"] / 1e6:.2f} {gap / 1000:.0f} | '
              f'{max([p["ping_100us"] for p in near], default=0) / 10:.0f} {max([p["ahead"] for p in near], default=0)} | '
              f'{signal_near(signals, a["t_us"] - 1000000, b["t_us"])} | ' +
              (', '.join(f'{(e["t_us"] - a["t_us"]) / 1000:+.0f}: {EVENTS.get(e["code"], e["code"])} {e["took_us"] / 1000:.1f}'
                         for e in around) or 'nothing'))


def percentile(values, q):
    if not values:
        return 0
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * q))]


def histogram(values, top):
    counts = {}
    for v in values:
        v = min(v, top)
        counts[v] = counts.get(v, 0) + 1
    return ' '.join(f'{k}{"+" if k == top else ""}:{counts[k]}' for k in sorted(counts))


def summary(path, text, polls, packets, events=(), signals=()):
    print(f'== {path}')
    for line in text.splitlines():
        print('  ' + line)
    plays = [p for p in polls if p['answer'] == 0]
    if not plays:
        return
    gaps = [p['gap_us'] for p in plays if p['gap_us']]
    busy = [p['gap_us'] - p['wait_us'] for p in plays if p['gap_us']]
    print(f'  polls {len(polls)}, frames played {len(plays)}; waits: ' +
          ', '.join(f'{ANSWERS[a]} {sum(1 for p in polls if p["answer"] == a)}' for a in (1, 2, 3, 4)))
    print(f'  a frame to the next (ms): median {percentile(gaps, .5) / 1000:.1f}, 95% {percentile(gaps, .95) / 1000:.1f}, '
          f'99% {percentile(gaps, .99) / 1000:.1f}, longest {max(gaps) / 1000:.1f}; over 21 ms: '
          f'{sum(g > 21000 for g in gaps)}, over 34 ms: {sum(g > 34000 for g in gaps)}')
    print(f'  the game\'s work a frame (the frame less its wait for the picture, ms): median '
          f'{percentile(busy, .5) / 1000:.1f}, 95% {percentile(busy, .95) / 1000:.1f}, 99% {percentile(busy, .99) / 1000:.1f}')
    print(f'  the picture a retrace late: {sum(p["retraces"] > 1 for p in plays)} frames; no spare time (<1 ms): '
          f'{sum(p["wait_us"] < 1000 for p in plays)}')
    print(f'  frames ahead of the opponent\'s inputs: {histogram([max(p["ahead"], 0) for p in plays], 8)}')
    rolled = [p for p in plays if p['rolled']]
    print(f'  rollbacks: {len(rolled)} ({histogram([p["rolled"] for p in rolled], 8)}); a frame with one (ms): median '
          f'{percentile([p["gap_us"] for p in rolled], .5) / 1000:.1f}, longest '
          f'{max([p["gap_us"] for p in rolled], default=0) / 1000:.1f}')
    by_depth = {}
    for p in rolled:
        by_depth.setdefault(min(p['rolled'], 8), []).append(p['gap_us'] - p['wait_us'])
    print('  the game\'s work in a frame with a rollback, by frames played again (median ms): ' +
          ' '.join(f'{k}:{percentile(v, .5) / 1000:.1f}' for k, v in sorted(by_depth.items())))
    plain = [p['gap_us'] - p['wait_us'] for p in plays if not p['rolled'] and not p['saves'] and p['gap_us']]
    saved = [p['gap_us'] - p['wait_us'] for p in plays if not p['rolled'] and p['saves'] and p['gap_us']]
    print(f'  the game\'s work in a frame (median ms): {percentile(plain, .5) / 1000:.1f} plain ({len(plain)}), '
          f'{percentile(saved, .5) / 1000:.1f} with a savestate ({len(saved)})')
    pings = [p['ping_100us'] / 10 for p in plays if p['ping_100us']]
    if pings:
        print(f'  ping (ms): lowest {min(pings):.1f}, median {percentile(pings, .5):.1f}, 95% {percentile(pings, .95):.1f}, '
              f'highest {max(pings):.1f}')
    offsets = [p['offset_100us'] / 10 for p in plays]
    print(f'  ahead of the opponent in time (ms): median {percentile(offsets, .5):.1f}, 5% {percentile(offsets, .05):.1f}, '
          f'95% {percentile(offsets, .95):.1f}')
    if len(packets) > 1:
        gaps = [b['t_us'] - a['t_us'] for a, b in zip(packets, packets[1:])]
        print(f'  the opponent\'s packets: {len(packets)}, {sum(p["added"] == 0 for p in packets)} with nothing new, '
              f'{sum(p["added"] > 1 for p in packets)} with more than one frame; from one to the next (ms): median '
              f'{percentile(gaps, .5) / 1000:.1f}, 95% {percentile(gaps, .95) / 1000:.1f}, 99% '
              f'{percentile(gaps, .99) / 1000:.1f}, longest {max(gaps) / 1000:.1f}')
    if events:
        print('  beside the game: ' + '; '.join(
            f'{EVENTS.get(c, c)} {sum(1 for e in events if e["code"] == c)} (median '
            f'{percentile([e["took_us"] for e in events if e["code"] == c], .5) / 1000:.1f} ms, longest '
            f'{max(e["took_us"] for e in events if e["code"] == c) / 1000:.1f})' for c in sorted({e['code'] for e in events})))
    if read.delays:
        print('  the delay changed during the game (s: to frames): ' +
              ' '.join(f'{x["t_us"] / 1e6:.1f}:{x["delay"]}' for x in read.delays))
    if signals:
        dbm = [s['dbm'] for s in signals]
        print(f'  Wi-Fi signal (dBm, {len(dbm)} readings): lowest {min(dbm)}, median {percentile(dbm, .5)}, highest '
              f'{max(dbm)}; by the SDK\'s levels (0-3): {histogram([level(x) for x in dbm], 3)}')
    big = [(a, b) for a, b in zip(packets, packets[1:]) if b['t_us'] - a['t_us'] > 100000]
    if big:
        starts = [a['t_us'] / 1e6 for a, b in big]
        merged = [starts[0]] + [t for prev, t in zip(starts, starts[1:]) if t - prev > 1.0]
        print(f'  the opponent\'s packets stopped for over 100 ms {len(big)} times, in {len(merged)} bouts at (s): ' +
              ' '.join(f'{t:.1f}' for t in merged))
        if len(merged) > 2:
            steps = [b - a for a, b in zip(merged, merged[1:])]
            print(f'    from one bout to the next (s): ' + ' '.join(f'{x:.1f}' for x in steps))
    # the worst seconds
    seconds = {}
    for p in plays:
        s = seconds.setdefault(p['t_us'] // 1000000, [0, 0, 0, 0])
        s[0] += 1
        s[1] += p['gap_us'] > 21000
        s[2] += p['rolled']
        s[3] = max(s[3], p['ping_100us'])
    waits = {}
    for p in polls:
        if p['answer'] != 0:
            waits[p['t_us'] // 1000000] = waits.get(p['t_us'] // 1000000, 0) + 1
    worst = sorted(seconds, key=lambda k: (-(seconds[k][1] + waits.get(k, 0)), k))[:8]
    print('  the worst seconds (second: frames played, late frames, waits, frames played again, ping ms): ' +
          '; '.join(f'{k}: {seconds[k][0]}, {seconds[k][1]}, {waits.get(k, 0)}, {seconds[k][2]}, {seconds[k][3] / 10:.0f}'
                    for k in sorted(worst)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--csv', action='store_true')
    ap.add_argument('--packets', action='store_true')
    ap.add_argument('--slow', action='store_true')
    ap.add_argument('--stalls', action='store_true')
    ap.add_argument('--events', action='store_true')
    ap.add_argument('--signal', action='store_true')
    args = ap.parse_args()
    for path in args.files:
        text, polls, packets = read(path)
        events = read.events
        signals = read.signals
        if args.csv:
            print(','.join(POLL_FIELDS + ['gap_us']))
            for p in polls:
                print(','.join(ANSWERS[min(p[k], 4)] if k == 'answer' else str(p[k]) for k in POLL_FIELDS + ['gap_us']))
        elif args.packets:
            print('t_us,frame,added,remote,ping_100us')
            for p in packets:
                print(','.join(str(p[k]) for k in ('t_us', 'frame', 'added', 'remote', 'ping_100us')))
        elif args.stalls:
            stalls(polls, packets, events, signals)
        elif args.events:
            print('t_us,what,took_us')
            for e in events:
                print(f'{e["t_us"]},{EVENTS.get(e["code"], e["code"])},{e["took_us"]}')
        elif args.signal:
            print('t_us,raw,dbm,level')
            for x in signals:
                print(f'{x["t_us"]},{x["raw"]},{x["dbm"]},{level(x["dbm"])}')
        elif args.slow:
            print('t_ms frame gap_ms work_ms ahead rolled saves save_us load_us packets retraces ping_ms')
            for p in polls:
                if p['answer'] == 0 and p['gap_us'] > 21000:
                    print(f'{p["t_us"] / 1000:.0f} {p["frame"]} {p["gap_us"] / 1000:.1f} '
                          f'{(p["gap_us"] - p["wait_us"]) / 1000:.1f} {p["ahead"]} {p["rolled"]} {p["saves"]} '
                          f'{p["save_us"]} {p["load_us"]} {p["packets"]} {p["retraces"]} {p["ping_100us"] / 10:.0f}')
        else:
            summary(path, text, polls, packets, events, signals)


if __name__ == '__main__':
    main()
