# UI / Interaction Design — tdeck-max-phone

> Design deliverable. Nothing here is implemented yet. It specifies the interaction
> model, key map, screen layouts and e-paper refresh strategy for the SIP phone that
> already registers, dials and carries two-way G.711 audio on real hardware.
>
> Scope: **UI only.** The telephony path (`tincan_uac`, RTP, ES8311) is working and is
> not redesigned here. Where the UI needs something the current APIs cannot express,
> it is listed in [§9 Prerequisites](#9-prerequisites--what-must-be-built-first) rather
> than assumed.
>
> Related issues: **#16** (font + status icons), **#17** (keypad digit mapping —
> this document is the design that unblocks it), **#18** (blocking `placeCall()`).

---

## 0. Two corrections to the brief, up front

### 0.1 The panel is 240x320 portrait, not 320x240

Three independent sources in-tree agree:

| Source | Value |
| :-- | :-- |
| `main/src/epaper_display.cpp:24-27` | `EPD_WIDTH 240`, `EPD_HEIGHT 320`, `EPD_BYTES_PER_ROW 30`, `EPD_BUF_SIZE 9600` |
| `T-Deck-MAX/lib/TDeckMaxBoard/src/TDeckMaxBoard.h:98-99` | `LCD_HOR_SIZE 240`, `LCD_VER_SIZE 320` |
| `T-Deck-MAX/examples/factory/factory.ino:38` | `GxEPD2_310_GDEQ031T10 // GDEQ031T10 240x320` |

LilyGO's factory firmware registers LVGL at 240x320 with **no rotation** (`factory.ino:255-256`),
which matches the physical form factor — a portrait brick with the QWERTY beneath the
screen. **All layouts below are 240 wide x 320 tall.** Designing at 320x240 would
require a rotation transform that does not exist in `epaper_display.cpp` (`set_pixel()`
indexes `s_fb[y * 30 + x/8]` directly) and would put the dial buffer on its side.

### 0.2 The digit keys already exist and are already on the keycaps' alt legends

The brief says "the mapping is not implemented, so manual dialling is impossible."
Correct — but the mapping is not *unknown*. LilyGO's own reference firmware carries it,
in two files that agree exactly:

- `T-Deck-MAX/examples/keypad/keypad.ino:17-22` — the base matrix
- `T-Deck-MAX/examples/factory/ui_deckpro.cpp:1415-1420` — `wifi_password_chat_map`,
  the symbol/number layer the factory firmware puts behind hold-UP

Overlaying the two produces this, which is **a telephone keypad already sitting inside
the QWERTY**:

```
        Q  W  E  R  T  Y  U  I  O  P          #  1  2  3  (  )  _  -  +  @
        A  S  D  F  G  H  J  K  L DEL   -->   *  4  5  6  /  :  ;  '  " DEL
       ALT Z  X  C  V  B  N  M  $  ENT       ALT 7  8  9  ?  !  ,  .  -  ENT
        .  .  .  .  . UP  0 SPC SYM UP        .  .  .  .  . UP  0 SPC SYM UP

                              1 2 3
        the 12 keys that matter:   4 5 6      * on A,  # on Q,  0 on its own key
                              7 8 9
                                0
```

`1`-`9` form a contiguous 3x3 block on `W E R / S D F / Z X C`; `0` is a dedicated
physical key; `*` and `#` are stacked on `A` and `Q` at the left edge. This is the
single most important fact in this document and it drives the whole design.

---

## 1. Design principles (why this looks the way it does)

1. **The dial pad is the default layer, not a shift layer.** A phone's primary input is
   digits. Making the user hold ALT/SYM for every digit of an 11-digit number is bad on
   any device and actively dangerous here — a held modifier is exactly the mechanism
   that broke in the factory firmware ([memory: keypad press/release inversion], and see
   [§9.1](#91-p1--fix-the-presrelease-polarity-blocking)). So in the dialling states,
   `W` types `1` with no modifier, matching the printed alt legend on the keycap.
2. **No alpha input in v1.** Nothing downstream can consume letters: `placeCall()` takes
   an extension that becomes `sip:<ext>@server`, drawbridge extensions are numeric, and
   the two documented dial targets are `9<number>` and `*777`. Alpha mode would cost a
   mode indicator, shift semantics, a 95-glyph font in a *large* size, and buy nothing.
   `ALT` and `SYM` are reserved and inert.
3. **The display is a persistent poster, not a screen.** E-paper holds its image at zero
   cost and repaints at high cost. Therefore: never refresh on a timer, never refresh to
   remove information, and use the free persistence to show things a backlit phone
   could not afford to — such as a permanent keypad cheat-sheet on the idle screen.
4. **Liveness goes on channels that can actually be live.** The panel cannot animate.
   Ringing, mute state and volume feedback are carried by the keyboard backlight
   (`BOARD_KEYBOARD_LED`, GPIO42) and the speaker, not by the panel.
5. **Every state advertises its own keys.** A QWERTY phone has no green/red call keys.
   A persistent 2-line hint bar showing what `ENT` and `DEL` do *right now* is not
   decoration; it is what makes the device operable without a manual.
6. **Assume the user cannot see the screen.** Speaker and mic are on-board and 2 cm
   apart with no AEC, so the natural posture during a call is holding the device to the
   ear — screen against the face. Every in-call control is therefore a corner or edge
   key, findable by touch, and produces non-visual feedback.

---

## 2. State / interaction model

```
                            ┌──────────────────────────────────────┐
                            │                                      │
   boot ──full──▶ ┌───────────────┐  digit (0-9,*,#)  ┌──────────┐ │
                  │     IDLE      │──────────────────▶│ DIALLING │ │
              ┌──▶│  (+ENDED      │◀──────────────────│          │ │
              │   │   variant)    │  DEL on empty     └──────────┘ │
              │   └───────────────┘  or hold-DEL           │       │
              │      │        ▲                            │ ENT   │
              │      │ ENT    │                            │       │
              │      │(redial)│ callEnded()                ▼       │
              │      │        │                     ┌───────────┐  │
              │      └────────┼────────────────────▶│  CALLING  │  │
              │               │                     │ (out) ⚠   │  │
              │               │                     └───────────┘  │
              │               │                       │        │   │
              │               │              answered │        │ fail / busy
              │  hasIncomingCall()                    ▼        ▼   │
              │   ┌────────────────┐              ┌────────┐  ┌────────────┐
              └───│    INCOMING    │─── ENT ─────▶│ IN CALL│─▶│   ENDED    │
      peer CANCEL │                │              │        │  │  / FAILED  │
      / DEL reject└────────────────┘              └────────┘  └────────────┘
                                                    DEL, or BYE from peer
```

### 2.1 State table

| State | Entered by | Exits | Panel |
| :-- | :-- | :-- | :-- |
| **IDLE** | boot; any call teardown | digit ⇒ DIALLING; `ENT` (redial, if a last number exists) ⇒ CALLING; `hasIncomingCall()` ⇒ INCOMING | 1 FULL on entry |
| **DIALLING** | first digit from IDLE | digit/`DEL` edit in place; `DEL` on empty or hold-`DEL` ⇒ IDLE; `ENT` (non-empty) ⇒ CALLING; incoming INVITE ⇒ INCOMING (buffer preserved) | 1 FULL on entry, then 1 PARTIAL(`B_NUMBER`) per coalesced edit |
| **CALLING** | `ENT` from DIALLING or IDLE-redial | `placeCall()==true` ⇒ IN CALL; `false` ⇒ ENDED(failed). **v1: no user exit** — see §2.3 | 1 FULL on entry, 1 FULL on exit |
| **INCOMING** | `uac.hasIncomingCall()` seen in `poll()` | `ENT` ⇒ answer ⇒ IN CALL; `DEL` ⇒ `reject()` ⇒ IDLE; `!hasIncomingCall()` (peer CANCEL) ⇒ IDLE | 1 FULL on entry |
| **IN CALL** | answer, or successful `placeCall()` | `DEL` ⇒ `hangup()` ⇒ ENDED; `callEnded()`/`!inCall()` ⇒ ENDED | 1 FULL on entry; PARTIAL(`B_SUB`) on mute toggle only |
| **ENDED / FAILED** | any call teardown | any key acts as IDLE; no timer | 1 FULL — **this refresh *is* the return to IDLE**, see §2.4 |

### 2.2 What each state does with a dial buffer that is already in progress

An INVITE can arrive mid-dial. Rule: **preempt, preserve, restore.** On DIALLING ⇒
INCOMING the buffer is copied to a holding slot; on reject, or when the caller gives up,
the phone returns to DIALLING with the buffer intact rather than to IDLE. Only a
completed call (which implies the user chose a different task) discards it.

### 2.3 ⚠ CALLING is not cancellable in v1 — and this is a telephony constraint, not a UI choice

`TincanUac::placeCall()` is documented as **blocking, bounded ~120 s worst case**, and it
drains the SIP socket inline (`tincan_uac.hpp:50-63`). While it blocks:

- the main loop does not run, so `tca8418_get_key()` is never called;
- the TCA8418's 10-deep event FIFO fills and then drops events;
- `hangup()` (which does know how to `CANCEL` a `Calling` dialog) is unreachable.

So the honest v1 CALLING screen is a **non-interactive wait screen**, and its hint bar
says so rather than offering a cancel that will not work. Two mitigations, in order of
cost:

- **Cheap and required:** drain the keypad FIFO immediately after `placeCall()` returns,
  so a frustrated user mashing keys during a 30 s wait does not have those presses
  replayed into the freshly-answered call.
- **Correct fix (scope of #18):** split into `placeCallBegin()` + a non-blocking
  `poll()`-driven progress check. That is a ~1-screen change to `TincanUac` and it is the
  only thing standing between this design and a cancellable outgoing call. The state
  machine above is drawn assuming it will land; until then, treat the CALLING⇒IDLE edge
  as absent.

`placeCall()` also returns only `bool` — there is no failure reason available, so the
FAILED screen can say "call failed" but not "busy" / "no answer" / "rejected" without an
API change.

### 2.4 ENDED is a rendering of IDLE, not a screen with a timer

The obvious design — show "Call ended 02:31" for three seconds, then return to idle —
costs **two** full refreshes, ~5 s of panel flashing, for information that vanishes. On
e-paper the cheaper and better answer is to fold the result into the idle screen: the
IDLE layout carries a `LAST` line and the number stays loaded for redial. One refresh,
information persists until it is superseded, no timer, no unrequested flash.

For the same reason there is **no dial-buffer inactivity timeout**. A stale buffer is
plainly visible on a persistent display and `DEL` clears it; an automatic transition
would repaint the whole panel while the user is not even looking at it.

### 2.5 The render-lag hazard, and the input grace window

The panel lags the state machine by seconds — `ui_render()` posts to a depth-1
`xQueueOverwrite` queue (`app_main.cpp:143-153`) and the render task then blocks for the
full refresh. A user can therefore press a key **in a state they have not seen yet**.
The dangerous case is concrete: the user is mid-dial with a finger on `ENT`, an INVITE
arrives, and `ENT`'s meaning silently changes from "dial" to "answer".

Rule: **on any transition that changes the meaning of `ENT` or `DEL`, discard queued key
events and ignore new ones until the new screen is actually on the panel** (or 3 s,
whichever comes first). This needs a render-completion signal — see
[§9.8](#98-p8--render-generation-acknowledgement). Applies to: ⇒INCOMING, ⇒IN CALL,
⇒CALLING.

---

## 3. Key map

### 3.1 Physical matrix (as decoded by `tca8418_get_key()`)

`key_num = (raw & 0x7F) - 1`, `row = key_num / 10`, `col = 9 - (key_num % 10)`
(`tca8418_keypad.cpp:181-189`). Base legends from `examples/keypad/keypad.ino:17-22`:

|  | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 | c9 |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| **r0** | Q | W | E | R | T | Y | U | I | O | P |
| **r1** | A | S | D | F | G | H | J | K | L | **DEL** |
| **r2** | **ALT** | Z | X | C | V | B | N | M | $ | **ENT** |
| **r3** | – | – | – | – | – | **UP** | **0** | **SPACE** | **SYM** | **UP** |

Alt/symbol legends from `ui_deckpro.cpp:1415-1420` (`wifi_password_chat_map`):

|  | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 | c9 |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| **r0** | # | 1 | 2 | 3 | ( | ) | _ | - | + | @ |
| **r1** | * | 4 | 5 | 6 | / | : | ; | ' | " | DEL |
| **r2** | ALT | 7 | 8 | 9 | ? | ! | , | . | – | ENT |
| **r3** | – | – | – | – | – | UP | – | SPACE | SYM | UP |

**The two UP keys are physically distinct** (`r3c5` and `r3c9`) even though LilyGO maps
both to the same character — they are separable at the matrix level, which is what makes
a two-key volume rocker possible with no modifier.

### 3.2 Assumed-physical keys I could not verify

| # | Assumption | How to settle it |
| :-- | :-- | :-- |
| **U1** | The `col = 9 - (key_num % 10)` reversal is correct | `CONFIG_TDECK_MAX_KEYPAD_DEBUG=y`, press `Q` then `P`, check the columns are 0 and 9 and not swapped. Already flagged in-repo (`tca8418_keypad.cpp:185-187`). |
| **U2** | Bit 7 of the event byte means *release*, not press | See §9.1 — this is the blocking one. |
| **U3** | The digit/`*`/`#` legends are **printed on the keycaps** | Look at the physical keyboard. If they are not printed, the idle-screen cheat-sheet (§4.1) carries the whole load and should stay on the dialling screen too. |
| **U4** | Which `UP` is physically left and which is right | KEYPAD_DEBUG: press the left shift, read the column. Vol-down goes on the left one. |
| **U5** | Whether `r3c0`–`r3c4` are physical keys at all | LilyGO maps them `NONE`. Sweep every physical key with KEYPAD_DEBUG and see if any lands there. **Nothing in this design binds them.** |
| **U6** | `r3c6` is the `0` key | Odd: the base map has `'0'` but *all three* factory text layers (lower/upper/chat) map it `NONE`. Either the factory deliberately disables it in text fields, or the base map is stale. Verify before trusting `0`. |
| **U7** | `r2c8` (`$`) has no telephony use | Not bound. |

### 3.3 The map, per state

Legend: **bold** = active, `·` = deliberately inert (press is consumed and ignored).
Inert keys must *not* fall through to any default — a stray letter must never enter the
dial buffer or end a call.

#### IDLE and DIALLING (the "dial layer")

|  | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 | c9 |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| **r0** | **`#`** | **`1`** | **`2`** | **`3`** | · | · | · | · | **`+`** | · |
| **r1** | **`*`** | **`4`** | **`5`** | **`6`** | · | · | · | · | · | **DEL** |
| **r2** | · (ALT, reserved) | **`7`** | **`8`** | **`9`** | · | · | · | · | · | **ENT** |
| **r3** | ? | ? | ? | ? | ? | **VOL−** | **`0`** | · | · (SYM, reserved) | **VOL+** |

- **digit / `*` / `#` / `+`** — append to the dial buffer. From IDLE this also enters
  DIALLING. Buffer capped at 20 characters; further presses are ignored (no beep — there
  is no beep path — the buffer simply stops growing, which is visible).
- **`+`** is included only because it is the printed legend on `O` and costs one glyph.
  Neither drawbridge nor the 3CX anchor consumes it today.
- **`ENT`** — IDLE with empty buffer: **redial the last dialled number** ⇒ CALLING. This
  is the designed replacement for the `POC_TEST_DIAL` bench hack, and it preserves the
  existing muscle memory (ENT-from-idle places a call). DIALLING with a non-empty
  buffer: dial it ⇒ CALLING. IDLE with no stored number: inert.
- **`DEL`** — tap: delete the last character; on an empty buffer, return to IDLE.
  Hold ≥ 600 ms: clear the whole buffer and return to IDLE in one action.
- **VOL±** — adjust the speaker volume. **Produces no panel refresh** (see §5.4); feedback
  is a 120 ms keyboard-backlight blink, and the level is drawn in the status bar the next
  time the status bar is painted for another reason.

#### CALLING (outgoing) — v1

|  | c0–c8 | c9 |
| :-- | :-- | :-- |
| **r0–r3** | · (all inert — the main loop is blocked inside `placeCall()` and cannot read them) | · |

The FIFO is drained on return. With #18 landed, `DEL` becomes **cancel** (`hangup()`
sends `CANCEL`) and the hint bar changes accordingly.

#### INCOMING

|  | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 | c9 |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| **r0** | · | · | · | · | · | · | · | · | · | · |
| **r1** | · | · | · | · | · | · | · | · | · | **DEL = reject** |
| **r2** | · | · | · | · | · | · | · | · | · | **ENT = answer** |
| **r3** | ? | ? | ? | ? | ? | **VOL−** (ring volume) | · | · | · | **VOL+** (ring volume) |

Every digit key is inert here **on purpose**: a user who was mid-dial when the call
arrived will still have digits under their fingers. Input is additionally suppressed
until the INCOMING screen is actually on the panel (§2.5).

#### IN CALL

|  | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 | c9 |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| **r0** | ○ | ○ | ○ | ○ | · | · | · | · | · | · |
| **r1** | ○ | ○ | ○ | ○ | · | · | · | · | · | **DEL = end call** |
| **r2** | · | ○ | ○ | ○ | · | · | · | · | · | · (reserved: hold/transfer) |
| **r3** | ? | ? | ? | ? | ? | **VOL−** | ○ | **SPACE = mute toggle** | · | **VOL+** |

- ○ = **reserved for DTMF, not bound.** There is no DTMF implementation in this firmware
  (no RFC 2833 telephone-event, no SIP INFO). These keys are inert today; they are marked
  here so nobody binds them to something else and blocks DTMF later. Anything requiring
  IVR navigation ("press 1 for sales") is currently impossible — flag for the user.
- **`DEL` = end call**, single press. It is the only destructive in-call key, it is a
  corner key findable by feel with the screen against your face, and nothing adjacent to
  it is bound during a call. (Alternative if accidental hangups show up on the bench:
  require a 400 ms hold. Not the default — a phone should hang up on one press.)
- **`ENT` is deliberately unbound in call.** Today's firmware hangs up on either `DEL` or
  `ENT` (`app_main.cpp:492`); that is one key too many for an irreversible action.
- **`SPACE` = mute**, a large centre key. Mute is a dangerous mode, so unlike volume it
  *does* repaint — one PARTIAL of `B_SUB` — and the keyboard backlight goes dark while
  muted, giving a glanceable indicator that costs no panel time. Mute is shown in `B_SUB`
  and **nowhere else**: an indicator in a band that a mute toggle does not repaint would
  go stale at exactly the moment it matters.

#### ENDED / FAILED

Identical to IDLE (it *is* the idle screen, §2.4). `ENT` redials the number that just
failed, which is the single most likely next action.

### 3.4 Global key rules

- **No auto-repeat, ever.** Holding a digit must not stream digits: each one would queue
  a panel refresh. Repeat is only defined for hold-`DEL`, and hold-`DEL` is a single
  discrete "clear" event, not a repeat.
- **Long-press threshold: 600 ms**, measured press-edge to release-edge. Requires the
  event API in §9.2 — the current `tca8418_get_key()` throws the release edge away
  (`tca8418_keypad.cpp:198`), so no long-press is implementable against it.
- **Inert keys are consumed silently.** No fallthrough, no logging at INFO level.
- Modifier keys `ALT` (r2c0) and `SYM` (r3c8) are reserved in every state. Do not bind
  them in v1; when alpha entry eventually arrives it will want them, and re-teaching a
  key is worse than leaving it dead.

---

## 4. Screen layouts (240 x 320, 1-bit)

**Reading the mockups.** Each box is exactly **30 characters wide x 20 rows tall** at
**8 px per character column and 16 px per row** — i.e. 240 x 320 true proportions. The
`y=` annotations give the pixel band each region occupies.

### 4.0 Band grid (this is the partial-refresh geometry)

All bands are **full-width (x = 0..239)** and non-overlapping. Full width is a deliberate
constraint, not laziness — see §5.3.

| Band | y range | rows | Contents |
| :-- | :-- | :-- | :-- |
| `B_STATUS` | 0 – 29 | 30 | identity + link state + volume; 2 px rule at y=28–29 |
| *(margin)* | 30 – 39 | 10 | never painted |
| `B_LABEL` | 40 – 79 | 40 | state name, F_UI at 2x |
| *(margin)* | 80 – 87 | 8 | never painted |
| `B_NUMBER` | 88 – 159 | 72 | the number, F_NUM auto-scaled |
| *(margin)* | 160 – 167 | 8 | never painted |
| `B_SUB` | 168 – 199 | 32 | secondary line (duration, MUTED, result) |
| `B_BODY` | 200 – 271 | 72 | keypad cheat-sheet (4 rows, y200–271), or call pictogram |
| `B_HINT` | 272 – 319 | 48 | 2 px rule at y=272–273, then two hint lines |

### 4.1 IDLE (and ENDED / FAILED, which is the same screen with different text)

```
 col 0         1         2         3
     0123456789012345678901234567890
    ┌──────────────────────────────┐
 r0 │1002 REG   WIFI       VOL 80  │  B_STATUS  y0-29   F_UI 1x
 r1 │──────────────────────────────│  rule      y28-29
 r2 │                              │
 r3 │           R E A D Y          │  B_LABEL   y40-79  F_UI 2x (16x32)
 r4 │                              │
 r5 │                              │
 r6 │                              │  B_NUMBER  y88-159 F_NUM scale 4
 r7 │      0 7 4 1 2 3 4 5 6 7     │   (last dialled = redial target)
 r8 │                              │
 r9 │                              │
r10 │                              │
r11 │      LAST CALL   02:31       │  B_SUB     y168-199 F_UI 1x
r12 │                              │
r13 │   1=W   2=E   3=R            │  B_BODY    y200-271 F_UI 1x
r14 │   4=S   5=D   6=F            │   persistent keypad cheat-sheet
r15 │   7=Z   8=X   9=C            │   (free on e-paper; costs nothing
r16 │   *=A   0=0   #=Q            │    to leave on screen forever)
r17 │──────────────────────────────│  rule      y272-273
r18 │ENT redial        DEL clear   │  B_HINT    y272-319 F_UI 1x
r19 │type a number to dial         │
    └──────────────────────────────┘
```

ENDED variant: `B_LABEL` = `CALL ENDED` / `CALL FAILED`, `B_SUB` = `02:31` or
`COULD NOT CONNECT`, everything else identical. FAILED cannot be more specific than that
without an API change (§2.3).

The cheat-sheet in `B_BODY` is the answer to open question **U3**: if the alt legends turn
out not to be printed on the keycaps, this block is the only thing teaching the mapping,
and it should then be duplicated onto the DIALLING screen (it fits — DIALLING's `B_BODY`
is otherwise empty).

### 4.2 DIALLING

```
    ┌──────────────────────────────┐
 r0 │1002 REG   WIFI       VOL 80  │  B_STATUS
 r1 │──────────────────────────────│
 r2 │                              │
 r3 │            D I A L           │  B_LABEL  F_UI 2x
 r4 │                              │
 r5 │                              │
 r6 │                              │  B_NUMBER  ← ONLY band that
 r7 │    9 0 7 4 1 2 3 4 5 6 7█    │    partial-refreshes in this
 r8 │                              │    state. █ = 4x28 px caret
 r9 │                              │
r10 │                              │
r11 │                              │  B_SUB — normally EMPTY, see below
r12 │                              │
r13 │   1=W   2=E   3=R            │  B_BODY (cheat-sheet retained
r14 │   4=S   5=D   6=F            │   — this is the state where it
r15 │   7=Z   8=X   9=C            │   earns its place)
r16 │   *=A   0=0   #=Q            │
r17 │──────────────────────────────│
r18 │ENT call          DEL erase   │  B_HINT
r19 │hold DEL to clear   9=outside │
    └──────────────────────────────┘
```

**`B_SUB` carries no live digit counter.** A per-keystroke count would repaint a second
band on every press, doubling the ghost debt and contradicting the annotation above. It is
used in this state for one thing only: once the buffer exceeds 13 characters and
`B_NUMBER` starts showing just the rightmost 13, `B_SUB` reads `20 DIGITS - SHOWING LAST 13`
so the truncation is never silent.

Crossing 13 → 14 characters is therefore a **two-band change, and by §5.2's own rule that
makes it a FULL refresh** — one flash, once, at the moment the display starts lying by
omission. Editing back down across the same boundary does the same. Every other edit in
this state stays a single `B_NUMBER` partial.

**`B_NUMBER` auto-scaling** (F_NUM is the 5x7 cell font; pitch = `5*scale + spacing`):

| buffer length | scale | glyph | pitch | fits |
| :-- | :-- | :-- | :-- | :-- |
| 1 – 8 | 5 | 25 x 35 | 30 px | 8 |
| 9 – 10 | 4 | 20 x 28 | 24 px | 10 |
| 11 – 13 | 3 | 15 x 21 | 18 px | 13 |
| 14 – 20 | 3 | 15 x 21 | 18 px | rightmost 13 shown; `B_SUB` states the true length |

Right-aligned, like a calculator, so the caret sits in a stable place and the digits you
just typed do not move. Vertically centred in the 72-row band.

### 4.3 CALLING (outgoing) — v1, non-interactive

```
    ┌──────────────────────────────┐
 r0 │1002 REG   WIFI       VOL 80  │
 r1 │──────────────────────────────│
 r2 │                              │
 r3 │       C A L L I N G          │  B_LABEL  F_UI 2x
 r4 │                              │
 r5 │                              │
 r6 │                              │
 r7 │    9 0 7 4 1 2 3 4 5 6 7     │  B_NUMBER F_NUM scale 3
 r8 │                              │
 r9 │                              │
r10 │                              │
r11 │                              │  B_SUB (empty — no ringback
r12 │                              │   state is observable, §2.3)
r13 │        ████████████          │  B_BODY: outgoing pictogram,
r14 │        ██        ██          │   a hollow ring, 96x56 px,
r15 │        ██        ██          │   drawn with fill_rect only
r16 │        ████████████          │   (no new glyph work)
r17 │──────────────────────────────│
r18 │connecting - please wait      │  B_HINT — says plainly that
r19 │keys are ignored until answer │   there is no cancel. With #18:
    └──────────────────────────────┘   "DEL cancel"
```

### 4.4 INCOMING

```
    ┌──────────────────────────────┐
 r0 │1002 REG   WIFI       VOL 80  │
 r1 │──────────────────────────────│
 r2 │                              │
 r3 │     I N C O M I N G          │  B_LABEL  F_UI 2x
 r4 │                              │
 r5 │                              │
 r6 │                              │
 r7 │      0 7 4 1 2 3 4 5 6 7     │  B_NUMBER: caller ID. NOTE:
 r8 │                              │   a non-numeric caller ID
 r9 │                              │   renders as gaps today (§7)
r10 │                              │
r11 │                              │  B_SUB
r12 │                              │
r13 │       ████████████████       │  B_BODY: solid 128x56 block —
r14 │       ████████████████       │   maximum black, unmistakable
r15 │       ████████████████       │   from across a desk, and it
r16 │       ████████████████       │   reuses the existing filled
r17 │──────────────────────────────│   pictogram idea
r18 │ENT answer      DEL reject    │  B_HINT
r19 │                              │
    └──────────────────────────────┘
```

Audible/tactile alert is what actually gets attention here — see §6. The panel is the
*confirmation*, not the alert.

### 4.5 IN CALL

```
    ┌──────────────────────────────┐
 r0 │1002 REG   WIFI       VOL 80  │  B_STATUS (no mute flag here —
 r1 │──────────────────────────────│   mute lives in B_SUB only)
 r2 │                              │
 r3 │      I N   C A L L           │  B_LABEL  F_UI 2x
 r4 │                              │
 r5 │                              │
 r6 │                              │
 r7 │      0 7 4 1 2 3 4 5 6 7     │  B_NUMBER F_NUM scale 3
 r8 │                              │
 r9 │                              │
r10 │                              │
r11 │           MUTED              │  B_SUB — the ONLY in-call
r12 │                              │   partial refresh (blank when
r13 │      ██████████████          │   unmuted)
r14 │      ██          ██          │  B_BODY: same ring as CALLING
r15 │      ██          ██          │   but with a filled centre bar
r16 │      ██████████████          │   = "connected"
r17 │──────────────────────────────│
r18 │DEL end call    SPACE mute    │  B_HINT
r19 │UP keys = volume              │
    └──────────────────────────────┘
```

**No live call timer.** See §5.5 for the arithmetic and the opt-in fallback.

### 4.6 `B_STATUS` field map (fixed pixel columns, so it repaints cheaply)

| x | width | Field | Values |
| :-- | :-- | :-- | :-- |
| 4 | 32 | own extension | `1002` (from `POC_SIP_EXT_SELF`) |
| 48 | 40 | registration | `REG` / `NOREG` |
| 104 | 32 | Wi-Fi | `WIFI` / `----` |
| 160 | 48 | volume | `VOL 80` — opportunistic, see §5.4 |
| 216 | 24 | *(reserved)* | blank |

**No mute flag in the status bar.** Mute is toggled with a `B_SUB` partial refresh, so an
indicator anywhere else would be wrong until the next full refresh. Only put a field in
`B_STATUS` if a change to it repaints `B_STATUS`, or if it is explicitly opportunistic
(volume is the sole exception, and it is a preference rather than a state you can be
caught out by).

**No battery indicator.** The BQ27220 fuel gauge is on the bus at 0x55 but no driver
exists in this firmware; a battery icon would be a lie. Same for signal strength — RSSI
is not exposed by `net_wifi.h`.

---

## 5. Refresh strategy

### 5.1 What the current driver does, and what is missing

`epd_full_refresh()` (`epaper_display.cpp:184-200`) does: reset → `PSR 0x1F` →
`POWER_ON 0x04` → `0x10` old framebuffer → `0x13` new framebuffer → `REFRESH 0x12` →
busy-wait → `POWER_OFF` + `DEEP_SLEEP`. It is a correct full refresh and it maintains
`s_old_fb`, which is exactly the "old data" a partial refresh needs.

Partial refresh is **supported by this panel** — LilyGO's vendor driver implements it
(`Display_EPD_W21.cpp:207-253`, `EPD_Dis_Part`) — but is not implemented here. Two
things are missing from the current init:

1. `EPD_Init()` writes `0xE0 = 0x02` and `0xE5 = 0x6E`; `EPD_Init_Part()` writes
   `0xE0 = 0x02` and **`0xE5 = 0x79`**. The current `epd_panel_init()` writes neither.
   `0xE5` is the *only* register whose value differs between the vendor's full and partial
   init, so it is what distinguishes the two modes. The vendor's source is undocumented on
   this point and no datasheet was consulted — treat the *reason* as inference and the
   *values* as copied verbatim from working code.
2. The window sequence: `0x91` (enter partial mode) → `0x90` (window: `x_start`,
   `x_end`, `y_start` hi/lo, `y_end` hi/lo, `0x01`) → `0x10` window-old → `0x13`
   window-new → `0x12` → power off.

Also required by the vendor, verbatim from their source: *"Partial refresh of background
display, this function is necessary, please do not delete it!!!"* — a full-screen base
map must be laid down before any partial refresh. In our terms: **a partial refresh is
only legal when `s_old_fb` genuinely matches what is on the glass**, which it does after
any `epd_full_refresh()`.

### 5.2 The rules

| Event | Refresh |
| :-- | :-- |
| Any **state transition** | **FULL** |
| Dial buffer edit (digit / backspace) | **PARTIAL** `B_NUMBER` |
| Mute toggle | **PARTIAL** `B_SUB` |
| Registration or Wi-Fi state change while idle | **PARTIAL** `B_STATUS` |
| Volume change | **none** (§5.4) |
| Optional in-call minute timer | **PARTIAL** `B_SUB` |
| Ghost debt reached the cap | **FULL** (promoted, see §5.6) |

The rule reduces to one sentence: **transitions are full, in-place edits are one band.**
A change that would touch two or more bands is by definition a transition and takes a
full refresh — three partials cost more panel time than one full and leave three times
the ghost debt.

### 5.3 Partial windows are full-width Y-bands only — deliberately

The vendor's `0x90` command writes `x_start`/`x_end` as raw single bytes after
`x_start -= x_start % 8`, and transfers `PART_COLUMN * PART_LINE / 8` bytes. Whether
those X parameters are **pixels or byte-columns** is not settled by reading the code, and
getting it wrong shifts or corrupts the window.

Restricting every window to the full panel width (x = 0..239, and 240 is a multiple of 8)
makes the design correct under *either* interpretation, keeps the byte count a clean
`30 * rows`, and lets a band be sliced straight out of `s_fb` / `s_old_fb` with no
per-row masking. The cost is repainting some blank pixels, which on e-paper is nearly
free. **Do not add X-windowing without first settling the units on the bench.**

One vendor detail to carry over: on the first partial after a full refresh, `EPD_Dis_Part`
sends `0xFF` for the window's old data rather than the real old bytes (`partFlag`).
We have accurate old bytes in `s_old_fb` and could send those, but the vendor's sequence
is the one known to work on this panel — follow it, and treat it as bench-tunable.

### 5.4 Volume never touches the panel

A volume keypress that costs a several-hundred-millisecond repaint is worse than no
feedback at all: it is slower than the ear, and it burns ghost debt during a call. So
volume feedback is a **120 ms keyboard-backlight blink** (`tca8418_set_backlight()`,
already implemented) and the numeric level is drawn into `B_STATUS` opportunistically —
whenever that band is painted for some other reason. The user hears the change
immediately; the screen catches up eventually. This is the right trade on e-paper.

### 5.5 There is no live call-duration timer

A 1 Hz seconds counter needs one partial refresh per second. Even at an optimistic
300 ms per partial, that is a panel that is refreshing ~30 % of the time for the entire
call, and — with any ghost rule at all — a full-screen flash every few seconds. It also
contends with nothing (the render task is separate) but wastes the panel's life for
information nobody is looking at, because during a call the screen is against the user's
face. **Verdict: not viable, and not wanted.**

Fallbacks, in order of preference:

1. **Default:** no in-call timer. Duration is measured with `esp_timer_get_time()` at
   answer and at teardown, and displayed **once**, on the ENDED screen — which is a full
   refresh that was going to happen anyway. Zero extra cost, and it is the only moment
   the number is actually useful.
2. **Opt-in (`POC_UI_CALL_TIMER_MINUTES`):** minute-granularity, one PARTIAL of `B_SUB`
   per minute. At 1 partial/min the ghost debt reaches the cap after 8 minutes, and the
   promoted full refresh lands mid-call. Acceptable only if a mid-call flash is
   acceptable. Off by default.

### 5.6 Ghost budget enforcement

LilyGO's FAQ rule is "force a full refresh after ~5 consecutive partials." Implemented
literally it fires mid-dial — typing an 11-digit number would flash the panel twice. Two
changes make it behave:

```c
static uint8_t s_partial_debt = 0;
#define GHOST_SOFT_LIMIT 5   /* pay the debt at the next natural transition */
#define GHOST_HARD_CAP   8   /* force a full refresh immediately            */
```

- Every partial: `s_partial_debt++`.
- Every full: `s_partial_debt = 0`. Since **all state transitions are full refreshes**,
  debt is normally paid off for free within a few seconds of it being incurred.
- If `s_partial_debt >= GHOST_HARD_CAP` when a partial is requested, promote that render
  to a FULL refresh instead.
- `GHOST_SOFT_LIMIT` is advisory: above it, any render that has a *choice* between
  partial and full picks full.

**And the reason the debt rarely reaches the cap: render coalescing.**

```
epaper_task:
    r = xQueueReceive(q, portMAX_DELAY)        // wait for work
    while (xQueueReceive(q, &r2, 250 ms) == pdTRUE)   // absorb the burst
        r = r2;                                        // newest wins
    render(r)
```

The depth-1 `xQueueOverwrite` queue already discards superseded frames
(`app_main.cpp:150-152`); adding a **250 ms settle** before rendering turns a burst of
typing into 2–4 partials instead of 11. This is the single highest-value change in the
whole refresh strategy, and it is about six lines.

### 5.7 Timing is unverified — measure it before tuning anything above

The README asserts a 2–3 s full refresh; nothing in-tree measures it, and **no partial
refresh has ever run on this hardware**, so every number in §5.5/§5.6 is reasoning from
an unmeasured baseline. Before tuning: bracket the busy-wait in `epd_full_refresh()` and
the new partial path with `esp_timer_get_time()`, log milliseconds, and run 20 of each.
The two numbers that decide the design are **partial-refresh wall time** and **how many
consecutive partials it takes before ghosting is objectionable on this specific panel**
(count them by eye — the FAQ's "5" is generic advice, not a measurement of this unit).

---

## 6. Alerting — what actually gets the user's attention

The panel cannot alert. Three non-panel channels exist on this board; only one is
currently wired up in this firmware.

| Channel | State | Use |
| :-- | :-- | :-- |
| **Speaker (ES8311)** | working — the audio self-test already synthesises a 1 kHz tone (`app_main.cpp:236-244`) | **Ringtone** on INCOMING; **ringback** during CALLING |
| **Keyboard backlight** (GPIO42) | working — `tca8418_set_backlight()` | Blink at ~2 Hz while INCOMING; dark while muted; 120 ms blink as volume feedback |
| **DRV2605 haptics** (I2C 0x5A) | power enable exists (`xl9555_set_motor_enable()`), **no driver** | Buzz on INCOMING. Requires a new driver — out of scope, flagged |

**The ring path needs an audio-task change.** `audio_task` only pumps when
`s_uac->inCall()` is true (`app_main.cpp:180-185`), so there is currently no code path
that can make a sound while a call is *ringing*. The main task must not do it either —
`audio_hardware_write_spk()` blocks on I2S and would stall SIP polling. Correct shape: an
atomic mode flag (`AUDIO_IDLE / AUDIO_RING / AUDIO_RINGBACK / AUDIO_CALL`) that
`audio_task` reads each iteration, generating a cadenced tone in the ring modes.
Remember `audio_hardware_set_amp(true)` while ringing, and back off on exit.

---

## 7. Font and glyph work this design requires

`epaper_display.cpp` has exactly one font: `s_digit_font`, a 5x7 cell covering **digits
0–9 only** (lines 69-80). `draw_digit_string()` renders any other character as a blank
gap (line 98-108) — so today `9*777` would display as `9 777`, and an alphanumeric caller
ID displays as nothing at all. Every piece of text in §4 therefore implies new work.
It is two fonts and it is small:

| Font | Status | Spec | Size |
| :-- | :-- | :-- | :-- |
| **F_NUM** | **extend existing** | Add `*` `#` `+` to the 5x7 cell font; index via a lookup instead of `c - '0'` | 3 glyphs x 7 bytes = **21 bytes** |
| **F_UI** | **new** | 8x16 bitmap, ASCII 0x20–0x7E, drawn at 1x (30 cols x 20 rows) and 2x (15 x 10) | 95 glyphs x 16 bytes = **1520 bytes** |

That is ~1.5 KB of flash for the entire UI. Everything else in the mockups — the rules,
the pictograms in `B_BODY`, the caret — is drawn with the existing `fill_rect()` and
needs no glyph data at all. This is deliberate: the pictograms are rectangles precisely so
that #16's "real status icons" never becomes a blocker.

**1-bit correctness.** Nothing above uses grey, dithering or anti-aliasing. Bitmap fonts
at integer scales stay crisp; strokes are ≥ 2 px (the rules) or ≥ 3 px (`scale`-multiplied
font stems) so nothing disappears into a single dithered row; there is no hairline
anywhere. The pictograms are solid black on white at maximum contrast.

### 7.1 Proposed display API (replaces `epaper_render_call_status`)

The current signature — `(caller_id, status, ptt_active)` — cannot express which band to
repaint, the volume, the mute flag, or the state as an enum (it currently *string-matches*
`"Call"`/`"Ring"` to decide the pictogram, `epaper_display.cpp:298-299`). Suggested shape:

```c
typedef enum { UI_IDLE, UI_DIALLING, UI_CALLING, UI_INCOMING, UI_INCALL, UI_ENDED } ui_screen_t;
typedef enum { B_STATUS, B_LABEL, B_NUMBER, B_SUB, B_BODY, B_HINT, B_ALL } ui_band_t;

typedef struct {
    ui_screen_t screen;
    char        number[24];        /* dial buffer, peer ext, or redial target  */
    char        self_ext[8];
    bool        registered, wifi_up, muted;
    uint8_t     volume;            /* 0..100                                    */
    uint32_t    last_call_secs;    /* 0 = none                                  */
    bool        last_call_failed;
} ui_model_t;

void     epaper_render(const ui_model_t *m, ui_band_t band); /* B_ALL => full refresh */
uint32_t epaper_render_generation(void);                     /* see §9.8              */
```

The render task keeps its depth-1 coalescing queue; only the payload type changes.

---

## 8. Touch verdict: **not used**

The CST3530 (I2C 0x1A — the part number the official LilyGO wiki gives; the I2C scan in
`app_main.cpp:92` still labels 0x1A `"CST328 touch"`, which is stale) stays undriven. This
is a decision, not an omission.

**Why not:**

1. **The feedback loop is broken by physics.** A tap needs acknowledgement inside ~100 ms
   to feel like it registered. The fastest acknowledgement this panel can produce is a
   partial refresh — hundreds of milliseconds, and unmeasured. Users who don't see a
   response tap again. On a dial pad that means a doubled digit; on an Answer button it
   means answer-then-hang-up.
2. **The feedback is not merely slow, it is droppable.** Renders go through a depth-1
   overwrite queue; under a burst of taps, intermediate frames are *designed* to be
   discarded. A touch UI whose visual acknowledgements can legitimately vanish is not a
   touch UI.
3. **It adds no capability.** Every action in §3 already has a physical key with a
   detent, a printed legend, and instant tactile confirmation. Touch would duplicate the
   keyboard, worse.
4. **It costs where the phone is most fragile.** A new CST3530 driver puts more traffic on
   the I2C bus shared with the ES8311 during a live call, for zero functional gain.
5. **It is wrong for the posture.** During a call the screen is against the user's face —
   any touch target is a cheek-dial waiting to happen, and there is no proximity sensor to
   suppress it (the LTR-553 is not part of the documented hardware and its code in the
   vendor repo is dead).

**The one case that could justify revisiting it:** a single, full-width **Answer** target
on the INCOMING screen (≥ 120 x 80 px), for the "grab the ringing device screen-first"
reflex. Even that is not recommended for v1 — `ENT` is a corner key, equally reachable,
and already correct. If it is ever added: exactly one target, never a keypad, and it must
tolerate its own visual feedback arriving seconds late.

**What would change this verdict:** a measured partial refresh comfortably under ~200 ms
for a small band, *and* a use case that a physical key cannot serve (scrolling a call
history of more than a handful of entries is the plausible one). Neither holds today.

---

## 9. Prerequisites — what must be built first

None of the key map is implementable against today's APIs. In dependency order:

### 9.1 ~~P1 — Fix the press/release polarity~~ **— RESOLVED 2026-08-13: there was no bug**

> **This section was wrong.** It is kept, corrected, because the reasoning that produced the
> wrong answer is a trap the next person will walk into as well — the vendor's own driver
> documentation states the opposite of what the hardware does.

**Measured on real hardware.** `CONFIG_TDECK_MAX_KEYPAD_DEBUG=y`, held one key for 4.58 s:

```
key[1] t=29144ms raw=0x8a bit7=1 key_num=9 -> r0c0    <- press (first event of the hold)
key[2] t=33728ms raw=0x0a bit7=0 key_num=9 -> r0c0    <- release, 4584 ms later
```

The first event of a hold is the press by definition, and it carries **bit 7 set**. So
`tca8418_keypad.cpp`'s `bool pressed = (raw & 0x80) != 0;` is **correct as written**, and
issue #35 is closed as invalid. Do not flip it.

**Why this document got it wrong**, since the same argument will look convincing again:

1. **The two vendor sources were not independent.** `examples/factory/peri_keypad.cpp:8-14`
   documents press = `0x01..0x50` / release = `0x81..0xD0` — but its comment opens with
   *"see Adafruit_TCA8418.cpp getEvent() docs"*. It is quoting
   `lib/Adafruit TCA8418/Adafruit_TCA8418.cpp:156-158`, not corroborating it. That made the
   evidence look 2-against-1 when it was 1-against-1 — and the lone dissenter,
   `examples/keypad/keypad.ino:86`, was the only one citing the **primary** source
   ("datasheet page 15 - Table 1"). It was right.
2. **The historical tiebreaker was bogus.** This section argued the factory file must be
   correct because its polarity explained a latched shift layer. It does not: the same file
   has a **fall-through bug** at `peri_keypad.cpp:167-175`, where two sequential `if`s should
   be `if`/`else if`. A release lands in the release branch (`k -= 129`), then falls into the
   press branch and is decremented *again* (`k -= 1`) and relabelled `PRESS`. So every
   release is reported as a press of the **adjacent key**, and no release ever reaches the
   consumer as a release — which latches a held modifier under *either* polarity.

   That bug also fully explains the "one press emits two characters" behaviour seen on
   modified vendor images: pressing `Q` (index 9) emits a phantom at index 8 = `W`, `V`
   (index 25) emits index 24 = `B`, and `P` (index 0) emits **no** phantom because index −1
   fails the `1 <= k <= 35` guard. **This repo does not inherit it** — it masks bit 7 before
   decoding, so press and release yield the same `key_num` and the `!pressed` filter drops
   exactly one of each pair.

**Consequence for this document:** nothing here is blocked. §9.2's event API is still needed
for hold-`DEL` and long-press (the release edge is discarded at `tca8418_keypad.cpp:198`),
but that is a missing *feature*, not a latent defect, and digit entry does not depend on it.

### 9.2 P2 — Event-based keypad API

`tca8418_get_key()` returns `char` and drops release events (`:198`). Long-press,
hold-to-clear and any modifier need both edges and a stable key identity:

```c
typedef struct { uint8_t key;  /* row*10 + col, stable scancode */
                 bool    down; } keypad_event_t;
bool tca8418_poll_event(keypad_event_t *ev);   /* false when the FIFO is empty */
void tca8418_flush(void);                      /* drain — used after placeCall() */
```

Returning a scancode rather than a `char` also removes the current ambiguity where `NUL`
means both "unmapped key" and "no key", which is what forced the separate `read_raw_event()`
drain path at init (`:145-153`).

### 9.3 P3 — Font work

Per §7: extend F_NUM with `*` `#` `+`; add the 8x16 F_UI ASCII font. Closes #16.

### 9.4 P4 — Partial refresh in `epaper_display.cpp`

Per §5.1/§5.3: `EPD_Init_Part` (`0xE0=0x02`, `0xE5=0x79`), the `0x91`/`0x90` window
sequence, full-width bands sliced from `s_fb`/`s_old_fb`, plus the ghost-debt counter and
the 250 ms coalescing window in the render task.

### 9.5 P5 — Runtime audio controls

No volume API exists — `POC_SPK_VOLUME` is applied once at init. Needs
`audio_hardware_set_out_vol(uint8_t pct)` and, for mute, either
`audio_hardware_set_mic_mute(bool)` or a flag in `audio_task` that substitutes silence
for the mic frame (cheaper, and it keeps the codec untouched mid-call).

### 9.6 P6 — Ring / ringback tone generation

Per §6: an atomic audio-mode flag consumed by `audio_task`, plus amp management. Without
this the phone rings **silently**, which for a phone is the single worst remaining defect
after digit entry.

### 9.7 P7 — (Recommended, #18) Non-blocking dial-out

`placeCallBegin()` + `poll()`-driven progress. Unlocks a cancellable CALLING state and
removes the ~120 s window in which the phone answers no SIP at all.

### 9.8 P8 — Render-generation acknowledgement

A monotonic counter the render task bumps *after* the panel finishes, readable by the main
loop, so §2.5's input grace window can wait for "the user can now see this screen" instead
of guessing with a fixed delay.

### 9.9 P9 — Persist the last dialled number

`nvs_flash_init()` already runs at boot. Storing the last dialled number makes `ENT`-from-
idle survive a reboot and lets `POC_TEST_DIAL` be deleted rather than merely bypassed.

---

## 10. Unverified ledger

Everything this design rests on that I could not confirm from source or a named document.

| # | Item | Status |
| :-- | :-- | :-- |
| U1 | Row/column decode, specifically the `col = 9 - (n % 10)` reversal | **RESOLVED 2026-08-13 — correct.** Measured: `Q` → `key_num 9` → `r0c0`, `P` → `key_num 0` → `r0c9`. The controller numbers this matrix right-to-left (P=1 … Q=10), which the reversal exactly compensates. |
| U2 | ~~Event bit 7 = release (not press)~~ | **RESOLVED 2026-08-13 — the opposite is true. Bit 7 set = PRESS**, so the existing code is correct and #35 is closed as invalid. Measured by holding one key 4.58 s: the first event carried `bit7=1`. See the corrected §9.1 for why two vendor sources say otherwise and are both wrong. |
| U3 | Digit / `*` / `#` legends are physically printed on the keycaps | Inferred from the factory firmware's symbol map. **Cannot be verified from code — the user can check by looking at the keyboard.** |
| U4 | Which of the two `UP` keys is physically left | **Unverified.** Decides which is vol-down. |
| U5 | Whether `r3c0`–`r3c4` are physical keys | **RESOLVED from vendor source — they are not.** See the `KEYPAD_PRESS_VAL_MAX` argument below. Nothing is bound to them. |
| U6 | `r3c6` is the `0` key | **RESOLVED from vendor source — yes.** `peri_keypad.cpp`'s `KEYPAD_PRESS_VAL_MAX 35` admits key indices 0–34 and rejects 35–39. Under `col = 9 - (idx % 10)` the five rejected indices are exactly `r3c4`…`r3c0`, the five `NONE` positions — 40 matrix positions minus 5 dead = 35 real keys, which *is* the constant. Index 33 = `r3c6` falls inside the accepted range, so the vendor's working firmware does treat it as `0`. |
| U7 | Full-refresh wall time (README claims 2–3 s) | **Unmeasured in-tree.** |
| U8 | Partial-refresh wall time, and the real ghosting threshold on this unit | **Never run on this hardware.** All of §5.5–§5.6 is reasoning from an unmeasured baseline. |
| U9 | Whether `0x90`'s X parameters are pixels or byte-columns | **Unresolved — and deliberately side-stepped** by using full-width bands only (§5.3). |
| U10 | Whether the panel accepts the partial sequence after the driver's `DEEP_SLEEP` | The partial path begins with its own reset + power-on, so it should. **Unverified.** |
| U11 | Touch IC is CST3530, not the CST328 the I2C scan prints (`app_main.cpp:92`) | Wiki-confirmed; the scan label and the LilyGO header it came from are stale. Cosmetic while touch is unused, but boot currently prints a wrong part name. |
| U12 | DRV2605 haptics | Power enable exists, **no driver**. Ring buzz is aspirational. |
| U13 | BQ27220 battery gauge | **No driver** — hence no battery indicator (§4.6). |
| U14 | DTMF | **Not implemented anywhere** (no RFC 2833, no SIP INFO). In-call digits are reserved and inert; IVR navigation is impossible today. |

---

## 11. Open questions for the user

1. **Are the number/symbol legends printed on the physical keycaps?** (U3.) If yes, the
   design is complete as written. If no, the idle-screen cheat-sheet becomes load-bearing
   and should also stay on the DIALLING screen.
2. **Is a mid-call full-screen flash acceptable?** It decides whether the opt-in
   minute-granularity call timer (§5.5) is worth building. Default assumption: no.
3. **Is DTMF needed?** If the phone must navigate an IVR ("press 1 for sales"), RFC 2833
   is a separate piece of telephony work and should be tracked before the in-call digit
   keys are designed around.
4. **Should `#18` (non-blocking dial-out) be pulled into this UI work?** It is the only
   thing preventing a cancellable outgoing call, and everything else in §9 is UI-local.
5. **Hang-up on a single `DEL` press, or a 400 ms hold?** Single press is specified;
   hold is the conservative option if the bench shows accidental drops.
6. **Call history beyond last-number redial?** Out of scope here. It is also the one
   feature that would genuinely re-open the touch question (§8).
