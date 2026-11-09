# Feeding strategies

## Built: `serve_on_demand` (2026-11-09)

The cat doesn't over-eat, but her food spoils quickly, so the food stays in the cold until she asks for it.
Feed mode `serve_on_demand` runs the `serve_and_return` cycle when the IR beam is broken, with no clock schedule
(details and entities: docs/esphome-firmware.md "Serve on demand").

1. Beam broken for 0.3 s with the lid closed: bring the next storage compartment (2 or 3, alternating) to the front.
2. Open the lid. No bell: the cat is already there.
3. Stay open for at least "On-demand minimum open" (3 min), then close once the beam has been clear for
   "Serve close after pet gone" (2 min). "Serve max open" (45 min) still applies.
4. Close with the usual anti-pinch checks and return the tray to rest compartment 1.
5. Ignore the beam for "On-demand cooldown" (5 min), then until it has been clear for 3 s.

## Built: `serve_and_return` (2026-11-01)

The timed cooling lid strategy below, refined by the user: wait for the cat instead of a fixed window.
Feed mode `serve_and_return` in polar_feeder (details and entities: docs/esphome-firmware.md "Serve and return").

Every "Serve interval" (default 120 min) from "Serve first meal" (08:00), when "Serve schedule enabled" is on:
1. Close the lid if needed, turn the next storage compartment (2 or 3, alternating) to the front, open the lid.
2. Ring the meal bell ("Call the pet at meals": volume 15, then every 2 min, 14 repeats) until the IR beam sees the cat.
3. Once the cat has been seen, close when the beam has been clear for "Serve close after pet gone" (2 min).
   Nobody within "Serve no-show close" (30 min), or "Serve max open" (45 min) reached: close anyway.
4. Close with the usual anti-pinch checks, then turn the tray back to the rest compartment 1 (food to the cold).

User decisions (2026-11-01): alternate compartments 2 and 3, the defaults above, bell at full volume to start
with, cooling kept on (switch now survives reboots; 8 h max-on cap set to 0 = no limit, as stock).
Tested on hardware: the no-show path (1 min test setting) end to end. Pet path and bell: with the user.

## Timed cooling lid strategy (proposed 2026-10-31, pending the water test)

**Idea (user):** the two rear tray positions sit in the refrigerated area; the front position, under the
lid opening, is not cooled. So keep all food in two compartments that normally rest at the rear, and at
each meal time bring one of them to the front, open the lid for a short window (e.g. 10 min), close the
lid, and return the food to the rear to stay cold. Fits a cat that eats a little and then wanders off.

**Observations behind it (user, 2026-10-31 morning, after the 4 h cooldown):** condensation in the cooling
area, so that part got properly cold; the rear two tray areas feel refrigerated, the front one does not; the
food NTC sits in the lid area, which is why it read only about 14.5 °C. To be confirmed by the water test.

### Mechanics (3-position tray)
- Rest position: the empty compartment (say 3) under the opening, food compartments 1 and 2 at the rear.
- Serve compartment 1: one step 3->1, open lid for the window, close, two steps 1->2->3 (compartment 2
  passes the opening with the lid closed). Serve compartment 2: two steps 3->1->2, window, one step 2->3.
- About 3 steps (about 10 s of motor time) per meal. Tray moves only with the lid closed (already enforced).
- Reverse rotation is possible in hardware (H-bridge 01) but untested, so forward-only stepping is the
  default; reverse would halve the travel.

### Comments
- **Food safety improves over lid-only:** food spends only the window outside the cold zone, and leftovers
  go back to the cold instead of sitting out until the next meal.
- **No portion control:** the cat can eat a whole compartment in one window. Fine for this cat; the planned
  scale is what would add portions.
- **Short windows can be missed** by a grazing cat. The meal call sound (polar_sound, call-the-pet) at the
  start of the window helps; more, shorter windows per day may suit a grazer better than few long ones.
- **Condensation:** cold food brought into warm air will sweat during the window and can carry water back
  to the rear. Probably harmless for wet food; watch for water pooling in the tray.
- **Repeated warm/cold cycles** of the same food over a day; keep total time at the front low (windows of
  a few minutes, not tens).
- **Which compartment per meal:** alternate (each holds half the day's food), or empty one before the other.
  Both should be options.
- **Anti-pinch and pet checks stay** on every lid close; with more lid cycles per day they matter more.
- **6/9/12 compartments:** the cold zone covers two of the three mark segments, so with sub-divided trays
  more small compartments could be stored cold and served one at a time; depends on the water test showing
  how far the cold zone extends.

### Firmware needed (not built yet)
A feed mode `present_and_return` in polar_feeder: storage compartments (set), rest position, window
length (per meal or global), compartment choice per meal (alternate / fixed / first non-empty), return
after the window or when the pet leaves, optional meal call. Reuses the existing tray stepping, lid
anti-pinch close and meal slots.

### Decision gate
Build it if the water test shows the rear positions hold food cold (target 4-5 °C) while the front does not.
