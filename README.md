# Nesso Presenter

Phase 2. An Arduino App that turns an **Arduino Nesso N1** into a presentation
remote: its keys change slide, tilting it moves a pointer, and its screen carries
a jump control and the running clock.

![The presenter, driven by a Nesso N1](assets/img/presenter.png)

It is a **separate App** from `nesso-companion` on purpose. The companion App is
a demo that registers everything the brick can do; this one is a real use case
that registers almost nothing — and that contrast is the point.

## What it asks the Nesso for

```
handshake sent: 4 sensor(s), 2 button(s), 0 modulino(s), 6 widget(s)
```

Four values: `pitch`, `roll`, `batteryVoltage`, `batteryLevel`. The
accelerometer, the gyroscope and every Modulino stay **asleep**, because nothing
here registered them. The companion App asks for ten sensors and five Modulinos
from the same firmware.

## Controls

| On the Nesso | |
| --- | --- |
| **KEY1**, the large button on the face | next slide |
| **KEY2**, on the edge | previous slide |
| **hold KEY1** (0.8 s) | fullscreen — see the caveat below |
| Tilt | moves the pointer |
| `Jump` on its screen | go to any slide |
| `Recentre` on its screen | take the current attitude as centre |
| `Pointer`, `Sensitivity` | pointer on/off, and how far a tilt travels |

**Fullscreen from the remote cannot be guaranteed, and that is the web platform,
not a bug here.** `requestFullscreen()` is only granted off a user gesture *in the
page*; a request arriving over a socket is not one, so the browser refuses it. The
firmware and the link are fine — measured, a hold produces `long_press` 810 ms after
`press` and the App asks immediately.

So the hold does two things:

- **Leaving** fullscreen needs no permission and always works. The remote can
  reliably get you out.
- **Entering** arms it: the deck shows "Fullscreen is ready — press any key or
  click here", and the next touch or keypress at the laptop carries it through.
  The prompt clears itself after twelve seconds.

If you would rather not touch the laptop at all, start the browser already
fullscreen and never toggle it:

```bash
chromium --kiosk http://<board-ip>:7000
```

KEY1 gets the action you do most because it is the big one under your thumb.
Recentring is a screen button rather than a long press: holding a key mid-sentence
is awkward, and a button can say what it does.

| In the browser | |
| --- | --- |
| `←` `→` `space` `PageUp/Down` | change slide |
| `f` or `b` | fullscreen |
| `p` | pointer |
| `r` | recentre the pointer |

Whichever side you touch, the other follows: the Nesso's screen shows the slide
number and clock, and its `Jump` stepper tracks the browser.

## What the UI shows, and why

The three readouts across the top are the things worth glancing at mid-talk:

- **Remote connected** — a dead remote mid-presentation is the failure that
  matters, so it is a dot and a sentence, not a buried status.
- **Battery voltage** — in **volts**, with the percentage small beside it. The
  gauge shipped uncalibrated and read 8% at 3.85 V, on a cell that is flat at
  3.3 V and full at 4.2 V; after one full charge on an external supply it
  learned, and now reads 99% at 4.09 V. Volts stay the headline anyway, because
  they are the reading that never needed a learning cycle. Below 3.55 V the
  readout turns red.
- **Clock** — elapsed time, start/pause/reset, mirrored onto the Nesso's screen.

Plus the classic presenter affordance: **up next**, so you know what is coming
without turning round.

If the App stops sending, the readouts dim rather than leave a stale number
looking live.

## The deck

`assets/slides.md`. Slides are separated by a line of `---`:

```markdown
# Slide title
![](img/slides/thing.svg)
> One line that carries the slide.
- a bullet, with **emphasis**
```

| Line | Becomes |
| --- | --- |
| `# text` | the title, with an accent rule under it |
| `![](path)` | the illustration — one per slide, and it splits the layout in two |
| `!live <kind>` | a live panel instead of an illustration, see below |
| ` ```…``` ` | a code block |
| `> text` | a lead line, larger and in full white |
| `- text` | a bullet |
| anything else | a muted paragraph |

### Live slides

Two slides are not drawings.

`!live pixels` mirrors a **Modulino Pixels** as eight dots, and drives the real
module while that slide is on screen — a comet if it is alone, a proximity bar
if a Distance module is attached too. Leave the slide and the LEDs go dark. So
the demonstration is simply to plug the module in during the talk: the Nesso
re-probes every three seconds, the panel switches from "waiting" to "attached,
and this slide is driving it", and the strip lights up.

`!live credits` reads the handshake back off the link — the values being
streamed, the buttons claimed, the widgets drawn, which Modulinos answered, and
how many slides were advanced from the remote versus the browser. Nothing there
is written down in advance.

Deliberately not a Markdown parser: that table is the whole grammar, which keeps
the deck editable by hand. Slide text is HTML-escaped before `**bold**` is
applied, since the deck is a file anyone can edit. It is read once at start-up,
so restart the App after editing.

### The illustrations

`assets/img/slides/*.svg`, hand-authored rather than stock images: they stay
crisp at any projector size, cost a couple of kilobytes each, carry no licensing,
and — being drawn for the slide — can show the actual mechanism instead of
decorating it. They share the App's palette, so a theme change is a find and
replace.

A slide with an illustration puts words on the left and the figure on the right;
without one, the words take the whole stage. On a portrait screen the split
stacks instead.

## Run it

```bash
arduino-app-cli app start user:nesso-presenter
```

Web UI on `http://<board-ip>:7000`, companion endpoint on `8077`.

> **Run one companion App at a time.** This App and `nesso-companion` both bind
> port 8077, and a Nesso can only link to one board at a time anyway. Using the
> same port is deliberate: switching Apps needs no reconfiguration on the Nesso.

## Notes

- The `nesso_companion` brick is **copied** into `bricks/`, because Custom Bricks
  live inside the App that uses them. A second consumer is the point at which it
  is worth publishing properly rather than copying — until then, changes have to
  be applied in both Apps.
- The pointer uses `pitch`/`roll` rather than raw acceleration: they are already
  gravity-referenced, so they hold still when the hand does. Smoothing is applied
  in Python, so the CSS deliberately has no transition on the pointer that would
  fight it.
- If the pointer moves the wrong way, the two signs are in `update_pointer()` in
  `python/main.py`; the Nesso's orientation in the hand decides them.
