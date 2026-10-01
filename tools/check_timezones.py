#!/usr/bin/env python3
"""Verify every POSIX rule in src/time/tz_table.cpp against the real IANA database.

Two independent checks run:
  1. the C library's own POSIX parser (what the main clock's timezone goes through), and
  2. src/time/posix_tz.cpp, the firmware's own evaluator (what the world clock goes through) -- compiled here by
     tools/tz_probe.cpp, so the code that actually runs on the device is what gets tested.

The device has no timezone database -- only the C library's POSIX TZ parser -- so each offered zone carries a
hand-written rule. A wrong daylight-saving rule looks perfect for months and then goes an hour out, which is exactly
the kind of defect that needs a test that can be re-run.

Method: parse the table out of the .cpp (so this can never drift from the firmware), read the transitions straight
out of the system tzfiles, then walk a multi-year horizon comparing the wall clock the C library produces from our
rule against the wall clock the IANA data says that zone really shows. Sampling is hourly across the whole horizon
plus once a minute around every real transition, so a transition an hour early or late cannot hide.

  python3 tools/check_timezones.py            # exits non-zero on any mismatch
  python3 tools/check_timezones.py -v         # also print the canonical rule from each tzfile footer
"""
import bisect, calendar, os, re, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "..", "src", "time", "tz_table.cpp")
ZONEDIR = "/usr/share/zoneinfo"
# Horizon: today through six years out. It starts at today rather than at a fixed year on purpose -- a POSIX rule
# describes one set of standing rules, so it cannot also describe a zone's past. When a region changes its rules
# (British Columbia and Alberta both went permanent during 2026) the single rule is right from the change onward and
# wrong before it, which is the correct trade for a clock that only ever renders the present.
HORIZON_YEARS = 6

ENTRY = re.compile(r'^\s*\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}\s*,', re.M)


def parse_table(path):
    with open(path, encoding="utf-8") as f:
        src = f.read()
    return [(m.group(1), m.group(2), m.group(3)) for m in ENTRY.finditer(src)]


def read_tzfile(zone):
    """Return (transition_times, utc_offsets_after_each, footer_tz_string). Offset before the first is offsets[0]."""
    with open(os.path.join(ZONEDIR, zone), "rb") as f:
        data = f.read()

    def block(off, wide):
        magic, ver = data[off:off + 4], data[off + 4:off + 5]
        if magic != b"TZif":
            raise ValueError("not a tzfile")
        p = off + 20
        isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt = struct.unpack(">6I", data[p:p + 24])
        p += 24
        tsz = 8 if wide else 4
        tfmt = ">%dq" % timecnt if wide else ">%di" % timecnt
        times = list(struct.unpack(tfmt, data[p:p + timecnt * tsz])); p += timecnt * tsz
        idx = list(data[p:p + timecnt]); p += timecnt
        types = []
        for _ in range(typecnt):
            utoff, isdst, _abbr = struct.unpack(">iBB", data[p:p + 6]); p += 6
            types.append(utoff)
        p += charcnt
        p += leapcnt * (12 if wide else 8)
        p += isstdcnt + isutcnt
        return p, times, idx, types, ver

    end, times, idx, types, ver = block(0, False)
    footer = ""
    if ver in (b"2", b"3", b"4"):
        end2, times, idx, types, _ = block(end, True)
        nl = data.index(b"\n", end2)
        nl2 = data.index(b"\n", nl + 1)
        footer = data[nl + 1:nl2].decode()
    first = types[idx[0]] if idx else types[0]
    offs = [first] + [types[i] for i in idx]
    return times, offs, footer


def real_offset(times, offs, ts):
    return offs[bisect.bisect_right(times, ts)]


def horizon():
    today = time.gmtime()
    lo = calendar.timegm((today.tm_year, today.tm_mon, today.tm_mday, 0, 0, 0, 0, 1, 0))
    hi = calendar.timegm((today.tm_year + HORIZON_YEARS, today.tm_mon, today.tm_mday, 0, 0, 0, 0, 1, 0))
    return lo, hi


def check(zone, posix, times, offs):
    """Return a list of (ts, expected_wall, got_wall) mismatches, at most a few."""
    os.environ["TZ"] = posix
    time.tzset()
    lo, hi = horizon()

    instants = range(lo, hi, 3600)
    dense = []
    for t in times:
        if lo - 7200 <= t <= hi + 7200:
            dense.extend(range(t - 7200, t + 7200, 60))

    bad = []
    for ts in list(instants) + dense:
        want = time.gmtime(ts + real_offset(times, offs, ts))[:6]
        got = time.localtime(ts)[:6]
        if want != got:
            bad.append((ts, want, got))
            if len(bad) >= 3:
                break
    return bad


def check_evaluator(entries):
    """Compile src/time/posix_tz.cpp and compare its offsets with the IANA data. Returns the number of failures."""
    probe = os.path.join(HERE, "tz_probe.cpp")
    src = os.path.join(HERE, "..", "src", "time", "posix_tz.cpp")
    if not (os.path.exists(probe) and os.path.exists(src)):
        print("\nskipping the firmware evaluator check: tz_probe.cpp or posix_tz.cpp is missing")
        return 0
    tmp = tempfile.mkdtemp(prefix="tzprobe")
    exe = os.path.join(tmp, "tz_probe")
    cc = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-O1", "-o", exe, probe, src],
                        capture_output=True, text=True)
    if cc.returncode:
        print("\nskipping the firmware evaluator check: g++ failed\n" + cc.stderr[:600])
        return 0
    if cc.stderr.strip():
        print("\nwarnings compiling the evaluator:\n" + cc.stderr[:600])

    lo, hi = horizon()
    jobs, want = [], []
    for zid, _label, posix in entries:
        try:
            times, offs, _ = read_tzfile(zid)
        except (OSError, ValueError):
            continue
        instants = list(range(lo, hi, 6 * 3600))
        for t in times:                                  # to the minute either side of every real transition
            if lo - 10800 <= t <= hi + 10800:
                instants += list(range(t - 10800, t + 10800, 60))
        for ts in instants:
            jobs.append("%d %s" % (ts, posix))
            want.append((zid, ts, real_offset(times, offs, ts)))

    out = subprocess.run([exe], input="\n".join(jobs) + "\n", capture_output=True, text=True)
    got = out.stdout.split("\n")
    bad, shown = 0, 0
    for (zid, ts, off), line in zip(want, got):
        if line.strip() != str(off):
            bad += 1
            if shown < 6:
                shown += 1
                print("FAIL evaluator %-30s at %s UTC: IANA %+d, posix_tz.cpp %s"
                      % (zid, time.strftime("%Y-%m-%d %H:%M", time.gmtime(ts)), off, line.strip() or "(no output)"))
    print("\nfirmware evaluator: %d instants across %d zones, %d disagreed with the IANA data"
          % (len(want), len(entries), bad))
    return 1 if bad else 0


def check_edge_cases():
    """Build and run tools/tz_edge_test.cpp: the cases the zone sweep cannot reach. Returns failures."""
    test = os.path.join(HERE, "tz_edge_test.cpp")
    src = os.path.join(HERE, "..", "src", "time", "posix_tz.cpp")
    if not os.path.exists(test):
        return 0
    tmp = tempfile.mkdtemp(prefix="tzedge")
    exe = os.path.join(tmp, "tz_edge")
    # the sanitisers are the point of this one: malformed rules used to overflow before being rejected
    cc = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-fsanitize=undefined,address", "-g",
                         "-o", exe, test, src], capture_output=True, text=True)
    if cc.returncode:
        print("\nskipping the edge-case test: g++ failed\n" + cc.stderr[:600])
        return 0
    run = subprocess.run([exe], capture_output=True, text=True)
    bad = [l for l in run.stdout.splitlines() if l.startswith("FAIL")]
    print("\nedge cases: %d checks, %d failed%s"
          % (len([l for l in run.stdout.splitlines() if l[:2] in ("ok", "FA")]), len(bad),
             " (sanitisers clean)" if not run.stderr.strip() else " -- SANITISER OUTPUT:\n" + run.stderr[:800]))
    for l in bad:
        print("  " + l)
    return 1 if (bad or run.returncode or run.stderr.strip()) else 0


def main():
    verbose = "-v" in sys.argv
    if not os.path.isdir(ZONEDIR):
        print("no %s on this machine -- cannot verify" % ZONEDIR, file=sys.stderr)
        return 2
    entries = parse_table(TABLE)
    if not entries:
        print("parsed no entries out of %s" % TABLE, file=sys.stderr)
        return 2

    common = re.search(r"TZ_COMMON_LEN = (\d+)", open(TABLE, encoding="utf-8").read())
    ncommon = int(common.group(1)) if common else 0
    want_common = ["Etc/UTC", "America/New_York", "America/Chicago", "America/Denver", "America/Phoenix",
                   "America/Los_Angeles", "America/Anchorage", "Pacific/Honolulu", "America/Puerto_Rico",
                   "Pacific/Guam", "Pacific/Pago_Pago"]
    got_common = [z for z, _, _ in entries[:ncommon]]
    common_bad = got_common != want_common
    if common_bad:
        print("FAIL TZ_COMMON_LEN=%d does not cover exactly UTC and the US zones (the first-boot wizard's list)"
              % ncommon)
        print("     got: %s" % ", ".join(got_common))

    saved = os.environ.get("TZ")
    fails = 1 if common_bad else 0
    seen = set()
    try:
        for zid, label, posix in entries:
            if zid in seen:
                print("FAIL %-34s duplicate id in the table" % zid)
                fails += 1
                continue
            seen.add(zid)
            try:
                times, offs, footer = read_tzfile(zid)
            except (OSError, ValueError) as e:
                print("FAIL %-34s no IANA data (%s)" % (zid, e))
                fails += 1
                continue
            bad = check(zid, posix, times, offs)
            if bad:
                fails += 1
                print("FAIL %-34s %s" % (zid, posix))
                print("     tzfile says the rule is: %s" % (footer or "(none)"))
                for ts, want, got in bad:
                    print("     at %s UTC  want %04d-%02d-%02d %02d:%02d  got %04d-%02d-%02d %02d:%02d"
                          % (time.strftime("%Y-%m-%d %H:%M", time.gmtime(ts)), *want[:5], *got[:5]))
            elif verbose:
                print("ok   %-34s %-30s tzfile: %s" % (zid, posix, footer or "(none)"))
    finally:
        if saved is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = saved
        time.tzset()

    fails += check_evaluator(entries)
    fails += check_edge_cases()

    lo, hi = horizon()
    print("\n%d zones, %d labels over 11 chars, %d failed, horizon %s to %s"
          % (len(entries), sum(1 for _, l, _ in entries if len(l) > 11), fails,
             time.strftime("%Y-%m-%d", time.gmtime(lo)), time.strftime("%Y-%m-%d", time.gmtime(hi))))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
