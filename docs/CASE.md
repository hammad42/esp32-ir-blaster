# 3D-printed enclosure notes

Optional, but a blaster spends its life on a shelf in a living room, so it may
as well not look like a science project.

---

## Dimensions

A DevKit v1 with the driver on a small piece of perfboard fits comfortably in:

```
  outer  90 × 55 × 30 mm
  walls  2 mm
  floor  2.5 mm
```

Standoffs for the DevKit: **M2, 4 mm tall**, at 20.3 × 48.3 mm centres (the
DevKit v1 hole pattern). Print them as part of the floor.

---

## The parts that actually matter

**The IR LED window.** Do not print over it. Either:

- leave a **6 mm hole** and let the 5 mm LED sit proud, or
- print the front wall **0.6 mm thin** over the LED in natural PLA — 940 nm
  passes through thin light-coloured plastic surprisingly well and it looks
  much tidier.

Black PLA blocks IR. If you print in black, you need an actual opening.

**Separate the LED from the receiver.** Put them on opposite faces, or at
minimum 5 cm apart with a printed rib between them. A blaster that hears itself
produces phantom captures during learning.

**Angle the LED up ~15°.** IR bounces well off white ceilings; a slight upward
tilt covers a room far better than aiming level at one appliance. If you fit two
LEDs, splay them 30° apart.

**Receiver aperture.** A 5 mm hole with the dome just behind the surface. A
short printed hood shades it from ceiling lights, which cuts noise noticeably.

**Vent the top.** A few 2 mm slots. The regulator on a DevKit runs warm and
this thing never gets switched off.

**Access the BOOT button.** Either a 4 mm hole you can poke with a paperclip —
recessed, so nothing presses it accidentally — or a printed plunger. Remember
it is the factory-reset button.

**Light pipes for the LEDs.** 3 mm holes, or 2 mm clear filament pushed through
and trimmed flush. Or print the front panel in natural PLA and let the LEDs glow
through 0.8 mm of it.

---

## Printing

| | |
|---|---|
| Material | PLA is fine indoors; PETG if it sits near a window |
| Layer height | 0.2 mm |
| Walls | 3 perimeters |
| Infill | 20% |
| Supports | none, if you design the lid as a separate part |

Print the box open-side-up and the lid flat. Snap-fit lids are more pleasant
than screws for something you may open again to add an LED.

---

## Mounting

A shallow keyhole slot in the back panel makes it wall-mountable, which is
usually the best position — high on a wall, facing into the room, bouncing off
the ceiling. Two 3M command strips work as well and do not need a drill.

---

## Ready-made alternatives

If you would rather not design one:

- Search **"ESP32 DevKit case"** on Printables or Thingiverse and add a 6 mm
  hole in the front for the LED — that is the entire modification.
- A **55 × 85 mm ABS project box** from any electronics shop, drilled, takes ten
  minutes and looks fine.
- A **white translucent** enclosure needs no LED window at all, which is the
  laziest good answer.
