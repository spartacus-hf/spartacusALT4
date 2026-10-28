# CW: keyer, Morse trainer, memories

This is the operator's guide to CW on the radio: the keyer (modelled on
Morserino-32), the Morse trainer for someone new to CW
with an iambic paddle, the receive zero-beat aids and the radio memories.
The trainer never transmits.

## Quick start for a CW newcomer

You need the paddle in the KEY jack, headphones, and an SD card that
`tools/flash_rev1.py` has filled. The trainer reads `CWPACK.BIN` (its
courses, words and the course's steps) and `HELP.BIN` (its instructions)
from the card. With no card, or an old pack, it says so and does not start.

MENU > CW > TRAINER > **START LEARNING** runs the whole course, the
alphabet and beyond, one lesson after the next. After START LEARNING you
only need the paddle: every step goes on by itself. Hold the knob (or FUNC)
to stop; the next START LEARNING carries on where you were.

The first time:

1. **Paddle check.** Press the lever you want to send dits. If the radio
   heard a dah, a click swaps the levers (DIT DAH SWAP). Then squeeze both
   levers and let go.
2. **Try the keyer.** Hold the dit lever (dits repeat), hold the dah lever
   (dahs repeat), squeeze both (they take turns). It goes on once you have
   done all three.

Then every lesson, in the order Morse teachers use (see *How the course
teaches* below):

1. **New characters.** Each is played three times and shown with its
   pattern; you key it back until you have it three times in a row.
2. **Listen.** Five groups of three of your characters, the new ones often;
   each is shown after it has played. Nothing to key.
3. **Sending drill.** The lesson's newest characters on the screen, five
   times each (KKKKK MMMMM); you key what you see. It trains your sending
   and is not part of the lesson's score, which is taken by ear.
4. **Echo.** Ten single characters by ear, then five words made of your
   characters (single characters while there are no words yet).
5. **Master the lesson.** Echo practice until 90 % of your last 50 first
   tries are right, and 90 % of your last ten or more of the newest
   character. Then the next lesson starts by itself with its new character.

Right is a quick double beep; wrong is one long low beep, with EXPECTED K
-.- HEARD R .-. on the screen. A first try you let run out of time counts
as a miss. The line under it shows your first-try score this session, your
weakest character and LAST 50: 46 RIGHT, the score the lesson is judged on.

Each new session starts at the top: the paddle check and keyer are done for
good, the newest characters are played again, and the listen, drill and
echo steps are your warm-up before the lesson goes on.

Once every character is in, the course goes on through **stages**, each
until 90 % of 50 first tries are right: groups of three characters, words,
CW abbreviations, call signs. Then the overall speed goes up 1 WPM
(SPACING WPM, never past LISTEN WPM: the gaps between characters shrink)
and the stages come round again. At full speed it ends in endless mixed
practice.

### How the course teaches

- **Koch** (LCWO, Morserino-32): characters at full speed from the start
  (LISTEN WPM 20) so you learn their sound, not their dots and dashes; the
  extra spacing between them gives you time (SPACING WPM 10, Farnsworth).
  Two characters first, one more each lesson, at 90 %.
- **LICW**: familiarity before recognition: you listen before you are asked.
- **CW Academy**: a daily sending drill; words, abbreviations and call signs
  as soon as you can; send what you see as well as what you hear.
- **Morserino-32's adaptive draw**: new and weak characters come up more
  often.

The order of characters is COURSE's (ADVANCED TRAINING): M32 (default) and
LCWO start with two and add one a lesson; CW ACADEMY takes its Beginner
sessions' groups (T E A N, then O I S 1 4, R H D L 2 5, ...); LICW three a
lesson (from its LICW ENTRY point); CUSTOM your own order. The course's
steps are a script on the card (`tools/cw_pack/steps.txt` in the pack),
not in the firmware.

## The trainer screen and its controls

- The title shows the activity, the lesson and a permanent **RF OFF**.
- Under it: LISTEN 20/10 WPM (character speed / overall speed), KEY 15 WPM,
  VOLUME.
- A band says whose turn it is: LISTEN, YOUR TURN, RIGHT, NOT QUITE,
  TIME UP, PAUSED, SESSION OVER.
- What was played (once you may see it) and what you keyed are drawn in
  large letters; live DIT and DAH badges show the levers.

| Control | Does |
|---|---|
| Turn the knob, or UP/DN | KEY WPM (your keying speed while training; the radio's CW SPEED is not touched) |
| Hold the knob and turn | Headphone volume |
| Click, or SEL | The action panel: NEXT, SUBMIT ANSWER, REPEAT, SKIP, PAUSE/RESUME, SWAP LEVERS, ADD NEXT CHARACTER, LISTEN WPM, SPACING WPM, END SESSION. While it is open the answer deadline waits, and keying closes it. |
| Hold, SEL held, or FUNC | Back to the TRAINER list |

## Activities (CW > TRAINER)

| Row | What it does |
|---|---|
| START LEARNING | The course above, from where you left it |
| ECHO PRACTICE | Hear a character or a group, key it back. PROMPT BY: sound, text or both |
| LISTEN | Hear groups and see each one after it; a lever pauses |
| LEARN NEW CHARACTER | Hear and see the lesson's newest character, key it three times |
| FREE KEYING | Key anything; the trainer writes what it hears |
| PADDLE CHECK | Which lever sends dits, swap them, squeeze and release |
| ADD NEXT CHARACTER | One more character in the lesson (the activities on this list recommend it, never add it by themselves; the course does it at 90 %) |
| DELETE PROGRESS | Forget lessons, scores and the course's steps done: click it twice (the second while CLICK AGAIN TO DELETE shows) |

**Lessons.** The courses are Koch orders: M32 (Morserino-32's), LCWO,
CW ACADEMY, LICW (with its carousel entry point) and CUSTOM, your own
order from `CW/CUSTOM.TXT`. As on Morserino-32, one draw in three comes
from the newest third of the lesson, and WEAK CHAR BOOST brings the
characters you miss more often. PRACTISE WITH picks characters, words, abbreviations, call
signs or a mix; words and abbreviations are limited to the lesson's
characters.

**When to add a character.** In the activities on this list: after at
least 50 first tries in the lesson, 10 of them on the newest character,
with at least 90 % right overall and 90 % on the newest, the screen shows
READY FOR MORE. You decide. START LEARNING adds it by itself.

**Answers.** A character or group is judged when you stop keying for ANSWER
WAIT S (2.5 s), or at SUBMIT. Seven or more dits clears what you keyed.
RETRIES (3) is how often a wrong answer is played again; only the first
try counts towards the score. THINK TIME S (5) is how long the trainer
waits for you to start. A session lasts SESSION MIN (5) minutes, then shows
its summary.

**Spacing.** LISTEN WPM is the character speed; SPACING WPM slows the
overall speed by stretching the gaps (ARRL Farnsworth). SPACING IN DITS
instead sets the gaps directly: CHAR SPACE DITS and WORD SPACE DITS.

## The keyer

| Setting | Default | Meaning |
|---|---|---|
| KEYER MODE | IAMBIC B | STRAIGHT, IAMBIC A, IAMBIC B, ULTIMATIC, NON-SQUEEZE |
| CURTIS DIT PCT / CURTIS DAH PCT | 75 / 45 | In Iambic B, how far into a dit / dah the other lever starts to count. Higher forgives a late release; 0 / 0 is classic Curtis B |
| MORSERINO B, CLASSIC B | actions | Set Iambic B with 75/45, or with 0/0 |
| CW WEIGHT PCT | 50 | Longer marks with shorter gaps; a dit plus its gap stays two dits |
| AUTO SPACE | off | After a character, wait until the space is 2-4 dits |
| KEY LATENCY | 0 | Eighths of a dit into a gap before a re-tap of the same lever counts |
| CW DEBOUNCE MS | 3 | Contact debounce, on press and release |

How the memories work, per mode: in Iambic A the other lever is remembered
from the end of a mark; in Iambic B from the Curtis point; holding a lever
repeats its element; in Ultimatic the lever closed last wins and every
closure gets its element; in Non-squeeze the first lever held keeps
control. The levers are read every millisecond, so a quick tap is never
lost however busy the radio is. A speed or weight change takes effect at
the next element: a mark is never cut short.

On the first boot of this firmware the keyer settings move once to the
defaults above. Your speed, swap, pitch, filter, volume and paddle
protection are kept.

## Receiving CW: zero-beat and SPOT

With CW DECODER on and a station locked, a tuning bar replaces the
CW DECODE label on the home screen. The marker shows the station's tone
against CW PITCH, a pixel per Hz up to 40 Hz either side. Right of the
centre tick means the station is above the dial: tune up. Within 10 Hz the
bar becomes a green **ZERO**.

**SPOT TONE** (ADVANCED CW > RECEIVE) plays the sidetone at CW PITCH for
4 s. Tune until the station sounds the same. Press again to stop it. It is
the sidetone only, never RF.

## Radio memories

Write up to eight lines into `CW/MEMORIES.TXT` on the card, for example:

```
CQ CQ DE DL1ABC DL1ABC K
TU 5NN <BK>
DL1ABC
73 TU <SK>
```

Use letters, digits, `. , : - / = ? @ +` and the prosigns `<AS> <KA> <KN>
<SK> <VE> <BK>` (`<AR>` and `<BT>` are `+` and `=`). A line holds at most 48
symbols, word spaces included. The file's first 511 bytes are read.

MENU > CW > **MEMORIES** (or the MEMORIES cell on the CW home row) lists
MEMORY 1-8; the help pane shows the selected line. A click sends it
through the keyer as if you keyed it: same speed, weight and break-in, and
the same safety holds (paddle arming, protection, TX MUTE, CW PRACTICE). The
memory stops when:

- you touch a lever (the element under way finishes);
- you click a memory row again;
- the radio leaves CW.

**REPEAT EVERY S** sends the memory again after that many seconds, unless
you touch a lever meanwhile. 0 sends it once.

## Files on the SD card

| File | Who writes it | What |
|---|---|---|
| `HELP.BIN` | `tools/flash_rev1.py` (from `tools/gen_help_text.py`) | Settings help, the trainer's instructions, the self test's details |
| `CWPACK.BIN` | `tools/flash_rev1.py` (from `tools/gen_cw_pack.py`) | Courses and their lesson groups, 2048 words and 245 abbreviations with the characters each uses, call sign prefixes, the course's steps |
| `CW/CUSTOM.TXT` | you | Your own course: characters in the order to learn them (spaces and repeats ignored, prosigns as `<SK>`) |
| `CW/MEMORIES.TXT` | you | The radio memories, one per line |

The console's `card` lists what the image needs and whether it is there.
The pack's data comes from Morserino-32 at commit `73a766f`
(GPL-3.0-or-later, `tools/cw_pack/README.md`).

**Your progress** (lesson per course, per-character scores, the course's
steps done and its stages passed) is a record in the card's settings
store, slot 193, not in the radio's flash. It is written at a lesson
change, a pause, a step's end and on leaving, never while you key. It is
part of BACK UP STORE, RESTORE and factory reset. A record written by a
newer firmware is kept, not overwritten (DELETE PROGRESS replaces it). A
record from v8.10-v8.11 is read as it is: the paddle check and keyer count
as done if the old guided session got past them. A store backup made by
this firmware does not restore on v8.09 (one more slot). If the card goes
missing during a session the trainer stops at the next prompt with NO SD
CARD: PROGRESS IS NOT SAVED; it never counts that as a wrong answer.

**SAVE PROGRESS** (TRAINER list, ON by default) OFF: the trainer starts
from your saved place but writes nothing back; leave and the next session
starts from the card's record again. For a guest, or to try something.
**DELETE PROGRESS** writes a fresh record whatever SAVE PROGRESS says.

## RF safety

The trainer owns the keyer while its page is open: practice and TX MUTE are
not touched, the keyer drives the sidetone only, and the receiver leaves
the headphones so prompts are clean. It refuses to start while you
transmit, calibrate, use USB disk mode, have the SD player open or stream
USB audio to the transmitter. On leaving, RF comes back only once both
levers have been open for 40 ms and the keyer is at rest. Nothing keyed in
the trainer can reach the air. An unarmed radio stays unarmed.

## Settings reference

**CW:** TRAINER, MEMORIES, CW SPEED WPM, CW PITCH HZ, HEADPHONE PCT, DIT DAH
SWAP, KEYER MODE, CW FILTER, CW DECODER, then ADVANCED CW and ADVANCED
TRAINING.

**ADVANCED CW:**
- **KEYER TIMING:** CURTIS DIT PCT, CURTIS DAH PCT, MORSERINO B, CLASSIC B,
  CW WEIGHT PCT, AUTO SPACE, KEY LATENCY, CW DEBOUNCE MS
- **BREAK-IN:** BREAK IN MODE, SEMI TAIL DITS, CW PRACTICE
- **RECEIVE:** CW AGC, SPOT TONE
- **KEY SAFETY:** PADDLE GUARD, ARM OPEN/BOTH/RELEASE MS, CW BOTH KEY,
  CW STUCK MS and the TX TEST pair
- **RF SHAPING:** CW RAMP MS, CW DRIVE, CW PHASE TRIM, CW THETA, CW THETA LAW

**TRAINER list:** the activities and DELETE PROGRESS, then ACTIVITY,
LESSON, GROUP LENGTH, LISTEN WPM, SPACING WPM, KEY WPM, HEADPHONE PCT,
SAVE PROGRESS.

**ADVANCED TRAINING:**
- **COURSE:** COURSE, LICW ENTRY, PRACTISE WITH
- **SPACING:** SPACING IN DITS, CHAR SPACE DITS, WORD SPACE DITS
- **ANSWERS:** RETRIES, ANSWER WAIT S, THINK TIME S, PROMPT BY, FEEDBACK TONE
- **SESSION:** WEAK CHAR BOOST, SPEED FOLLOWS (in echo practice the listen
  speed goes up a WPM for a right answer and down for a wrong one, within
  10 WPM of LISTEN WPM; never the radio's CW SPEED), SESSION MIN

**MEMORIES:** MEMORY 1-8, REPEAT EVERY S.

Setting ids keep their console numbers (`set list`): the old trainer's
ids are reused and new ones appended.

## Not in this firmware yet

A file player for text files from the card and QSO practice were planned
next. The firmware has no flash left for them while it stays smaller than
main (v8.09 + DIGI), so they wait.
