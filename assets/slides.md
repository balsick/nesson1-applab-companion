# Nesso N1 Companion

A second screen, a remote control and a handful of sensors,
on the other side of a WebSocket.

Hold the Nesso. Press KEY1 to move on.

---

# The idea

The App declares what it wants. The Nesso does only that.

- Registering a callback is what turns a sensor **on**
- An App that never asks for the gyroscope leaves it asleep
- The whole list goes over in one `config` frame

---

# Finding each other

- The Nesso browses mDNS for `_arduino._tcp`
- Then asks each board `GET /info` on the companion port
- Boards running a companion App get a green dot
- You pick one on the touch screen

No pairing, no addresses to type.

---

# The handshake

The board answers `hello` with everything it needs:

- which sensors, at which rate
- which buttons
- which Modulinos
- which widgets to draw

Only then does the Nesso start any of it.

---

# This deck is the remote

- **KEY1** next slide
- **KEY2** previous slide
- **hold KEY1** blank the screen
- **hold KEY2** recentre the pointer
- **Jump** on the Nesso screen goes anywhere

---

# Point at things

Tilt the Nesso and the pointer follows.

- pitch and roll, not raw acceleration
- smoothed, because hands shake
- recentre whenever you change your grip
- **Sensitivity** sets how far a tilt travels

---

# Modulinos

Plug one into the Qwiic connector and it is
picked up within a few seconds. No re-linking.

- Knob, Buttons, Movement, Distance
- Pixels as an output
- Absent ones simply stay quiet

---

# What it cost

The Nesso side is a generic executor:

- it renders whatever widgets the App declares
- it writes whatever the App sends to the Modulinos
- changing this deck never reflashes anything

Which is why Phase 2 was an App restart, not a rebuild.
