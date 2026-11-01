#!/usr/bin/env python3
"""Plot a cooldown CSV from tools/coollog.py-style logging (time, Food temperature, Peltier, ..., duty).
Usage: plot_cooldown.py captures/<log>.csv docs/img/<name>.png [--room 23] [--title "..."]
Two stacked panels sharing time (temperature, Peltier duty) - never a dual axis."""
import argparse, csv, datetime as dt
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

SURFACE, INK, INK2, GRID, SERIES = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df", "#2a78d6"

ap = argparse.ArgumentParser()
ap.add_argument("csv"); ap.add_argument("out")
ap.add_argument("--room", type=float); ap.add_argument("--title", default="Cooldown")
ap.add_argument("--date", default="2026-10-30")
ap.add_argument("--off-label", default="Peltier off\n(60 min limit)")
ap.add_argument("--duty-note", default="rolling window began at the 23:29 reboot, shortly before the Peltier came on, so it ramps up")
ap.add_argument("--ambient", help="CSV time,ambient_c (tools/fetch_ambient.sh): plotted as the room line")
ap.add_argument("--points", default="", help="hand readings: 'HH:MM=value=label=group;...' group = slot 2 or 3 colour")
ap.add_argument("--legend", action="store_true")
ap.add_argument("--no-endlabels", action="store_true", help="skip the start/end value labels on the air line")
a = ap.parse_args()

rows = list(csv.DictReader(open(a.csv)))
d0 = dt.date.fromisoformat(a.date); t, temp, duty, on = [], [], [], []
prev = None
for r in rows:
    hh, mm, ss = map(int, r["time"].split(":"))
    ts = dt.datetime.combine(d0, dt.time(hh, mm, ss))
    if prev and ts < prev: d0 += dt.timedelta(days=1); ts += dt.timedelta(days=1)
    prev = ts
    def f(k):
        try: return float(r[k])
        except (ValueError, KeyError): return float("nan")
    t.append(ts); temp.append(f("Food temperature")); duty.append(f("Peltier duty (1 h)")); on.append(r["Peltier"] == "True")

plt.rcParams.update({"font.size": 10, "text.color": INK, "axes.labelcolor": INK2, "xtick.color": INK2,
                     "ytick.color": INK2, "axes.edgecolor": GRID, "font.family": "DejaVu Sans"})
fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(9, 6.2), height_ratios=[3, 1.4], facecolor=SURFACE)
for ax in (ax1, ax2):
    ax.set_facecolor(SURFACE); ax.grid(True, color=GRID, linewidth=0.8); ax.set_axisbelow(True)
    for s in ("top", "right"): ax.spines[s].set_visible(False)

ax1.plot(t, temp, color=SERIES, linewidth=2, solid_capstyle="round", label="Compartment air (feeder NTC)")
if a.ambient:
    at, av = [], []
    for r in csv.DictReader(open(a.ambient)):
        try: av.append(float(r["ambient_c"]))
        except ValueError: continue
        at.append(dt.datetime.fromisoformat(r["time"]))
    ax1.step(at, av, where="post", color="#8a8984", linewidth=1.5, label="Room (kitchen sensor)")
SLOT = {"2": "#eb6834", "3": "#1baf7a"}
seen = set()
for spec in filter(None, a.points.split(";")):
    parts = spec.split("="); hm, val, lab, grp = parts[:4]; side = parts[4] if len(parts) > 4 else "L"
    hh, mm = map(int, hm.split(":")); x = dt.datetime.combine(t[0].date(), dt.time(hh, mm)); v = float(val)
    glabel = {"2": "Water, front compartment (Thermapen)", "3": "Water, rear compartments (Thermapen)"}[grp]
    ax1.plot([x], [v], "o", color=SLOT[grp], markersize=8, markeredgecolor=SURFACE, markeredgewidth=2, zorder=5,
             label=None if glabel in seen else glabel)
    seen.add(glabel)
    if lab:
        ax1.annotate(lab, (x, v), xytext=(-10 if side == "L" else 10, 0), textcoords="offset points",
                     ha="right" if side == "L" else "left", va="center", color=INK, fontsize=9)
if a.room is not None:
    ax1.axhline(a.room, color=INK2, linewidth=1, linestyle=(0, (4, 3)))
    ax1.annotate(f"room {a.room:g} °C", (t[len(t) // 2], a.room), xytext=(0, 4), textcoords="offset points",
                 ha="center", va="bottom", color=INK2, fontsize=9)
# Direct labels: start and end only.
i0 = next(i for i, v in enumerate(temp) if v == v); i1 = max(i for i, v in enumerate(temp) if v == v)
for i, ha, dy in (() if a.no_endlabels else ((i0, "left", 12), (i1, "right", -10))):
    ax1.plot([t[i]], [temp[i]], "o", color=SERIES, markersize=6, markeredgecolor=SURFACE, markeredgewidth=2)
    ax1.annotate(f"{temp[i]:.1f} °C", (t[i], temp[i]), xytext=(0, dy), textcoords="offset points",
                 ha=ha, va="bottom" if dy > 0 else "top", color=INK, fontsize=9)
off = [t[i] for i in range(1, len(on)) if on[i - 1] and not on[i]]
for x in off:
    for ax in (ax1, ax2): ax.axvline(x, color=INK2, linewidth=1, linestyle=(0, (2, 2)))
    ax1.annotate(a.off_label, (x, (ax1.get_ylim()[0] + ax1.get_ylim()[1]) / 2),
                 xytext=(-6, 0), textcoords="offset points", ha="right", va="center", color=INK2, fontsize=9)
ax1.set_ylabel("Temperature (°C)" if (a.ambient or a.points) else "Food compartment air (°C)")
if a.legend:
    ax1.legend(loc="upper right", frameon=False, fontsize=9, labelcolor=INK2)
ax1.set_title(a.title, loc="left", fontsize=12, color=INK, pad=10)

ax2.plot(t, duty, color=SERIES, linewidth=2)
ax2.set_ylim(0, 100); ax2.set_ylabel("Peltier duty,\nrolling 1 h (%)")
ax2.annotate(a.duty_note, (t[len(t) // 6], 3), xytext=(6, 0),
             textcoords="offset points", ha="left", va="bottom", color=INK2, fontsize=8.5)
ax2.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
ax2.set_xlabel("Time (local)")
fig.tight_layout()
fig.savefig(a.out, dpi=150, facecolor=SURFACE)
print("wrote", a.out)
