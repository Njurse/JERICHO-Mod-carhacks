# Car sirens: which sample a car plays, and where it lives

This is the full contract behind `CarHasSiren()` in `src_rebuild/Game/C/gamesnd.c`.
It covers native cars, cross-city imports, the two different "third sample"
layouts, how a D2 import gets its own city's siren, and what happens when the
car sound bank is rebuilt.

The short version:

- A car's **engine, idle and horn** are three consecutive samples in
  `SOUND_BANK_CARS`, addressed as `block * 3`, `block * 3 + 1`, `block * 3 + 2`.
- A car's **siren** is a separate sample. `CarHasSiren(slot)` returns a packed
  `(bank, sample)` value; it is not the car bank's generic third sample.
- Driver 1/imported custom cars put their siren in the **car bank** (the baked
  third sample).
- Driver 2 cars put the cop siren in **`SOUND_BANK_VOICES`**. A D2 car imported
  from another D2 city must use that **source city's** voice sample, not the
  host level's sample 0.

## 1. The two banks

| Bank | What it holds | How it is addressed |
| --- | --- | --- |
| `SOUND_BANK_CARS` | engine, idle, horn (and, for a D1/custom import, the siren) | a per-slot block: `block * 3 + 0/1/2` |
| `SOUND_BANK_VOICES` | cop speech, stock D2 cop sirens, and the imported D2 siren banks | a sample index; the host city's siren is sample 0 |

`CarHasSiren()` returns `M_SHRT_2(bank, sample)`, i.e. the bank in the high
byte and the sample in the low byte. `StartPlayerCarSounds()` and
`DoDopplerSFX()` unpack it with `(siren & 0xff00) >> 8` and `siren & 0xff`.

The host city's siren bank is loaded in `LoadLevelSFX()`:

```c
LoadBankFromLump(SOUND_BANK_VOICES, CopSirenBankForCity(GameLevel));
cop_siren_sample[GameLevel] = 0;
```

Because it is the first thing loaded into `SOUND_BANK_VOICES`, the host siren is
sample **0**. The cop phrase banks are loaded after it, and the imported D2
siren banks are appended after those, so appending never shifts the hardcoded
phrase sample indices.

## 2. `CarHasSiren(slot)` resolution order

`slot` is a **resident car-slot index**, the value in `cp->ap.model`. It is not
the car-model number in `residentCarModels[slot]`.

The order is deliberate:

1. **Bounds.** A slot outside `0..MAX_CAR_RESIDENT_MODELS-1` has no siren.
2. **Special car slot.** `SPECIAL_CAR_SLOT` keeps its mission-specific
   `SOUND_BANK_SFX` samples and its stock fallback.
3. **Driver 2 import.** If the slot's source city is a D2 city (`0..3`) and the
   slot does not own an embedded imported bank:
   - model `0` (the D2 police car) returns
     `(SOUND_BANK_VOICES, cop_siren_sample[sourceCity])`;
   - every other model returns `0`.
   This is the source-city fix. It deliberately **does not** fall through to
   the stock “model 0 is a cop” rule, because for an import that rule would
   select the *host* city's siren.
4. **Driver 1/custom import.** If the source city is `>= CITY_D2_COUNT`, or the
   slot owns an embedded imported bank, the answer comes from
   `ImportedCarHasSiren(slot)`:
   - `GetCarImportSiren(city, model)` must be true (the bake's `LUMP_CAR_SIRENS`
     mask);
   - the siren is the imported car bank's third sample, i.e.
     `(SOUND_BANK_CARS, car_slot_sample[slot] * 3 + 2)`;
   - a civilian import returns `0`; it never borrows the host city's siren.
5. **Native car.** A native model-0 police car returns
   `(SOUND_BANK_VOICES, 0)` — the host level's siren. Any other native model
   returns `0`.

A D1/custom import with **no embedded sound bank** also returns `0`: there is
no correct source-city siren sample to play, and a wrong siren is worse than a
civilian car staying silent. Re-bake the city to get its siren.

## 3. The D2 per-city siren mapping

The four D2 cities are not a simple `69 + city` table. The stock loader used a
bitwise mapping; `CopSirenBankForCity()` now isolates it:

```c
static int CopSirenBankForCity(int city)
{
    if (city & 2)
        return SBK_COP_SIREN_START + (city & 1) * 2;

    return SBK_COP_SIREN_START + (city & 3);
}
```

| City index | City | Cop siren bank |
| --- | --- | --- |
| `0` | CHICAGO | `69` |
| `1` | HAVANA | `70` |
| `2` | VEGAS | `69` (shares CHICAGO) |
| `3` | RIO | `71` |

`SBK_COP_SIREN_START` is `69`. Chicago and Vegas share a siren; Havana and Rio
have their own. This helper is used for the host city's siren and for every
imported D2 source city, so the host mapping is unchanged.

## 4. Loading an imported D2 siren

`LoadImportedCopSiren(city)` is called for a D2 city when a resident slot has
that source city and model `0`:

- At level load, after the cop phrase banks and before `ShowLoading()`:
  `LoadLevelSFX()` loops the resident slots and calls it for every D2 model-0
  import.
- At runtime, `LoadCarSoundForSlot()` calls it for a model-0 D2 source. This
  covers a car hot-loaded after the level started.

The loader is idempotent:

```c
if (cop_siren_sample[city] >= 0)
    return;

bank = CopSirenBankForCity(city);

for (i = 0; i < CITY_D2_COUNT; i++)
    if (cop_siren_sample[i] >= 0 && CopSirenBankForCity(i) == bank)
    {
        cop_siren_sample[city] = cop_siren_sample[i];
        return;                    // shared bank: reuse the first load
    }

before = GetSoundBankSampleCount(SOUND_BANK_VOICES);
LoadBankFromLump(SOUND_BANK_VOICES, bank);
after  = GetSoundBankSampleCount(SOUND_BANK_VOICES);

if (after <= before)
{
    cop_siren_sample[city] = -1;   // load failed
    printWarning("gamesnd: %s cop siren bank %d could not be loaded\n",
                 LevelNames[city], bank);
    return;
}

cop_siren_sample[city] = before;
printInfo("gamesnd: %s cop siren -> SOUND_BANK_VOICES sample %d (bank %d)\n",
          LevelNames[city], before, bank);
```

Notes:

- The exact sample index is whatever `GetSoundBankSampleCount()` reports after
  the host siren and the cop phrase banks. In a RIO-host run the observed
  values were CHICAGO `18`, HAVANA `19`, VEGAS `18` (shared with CHICAGO), and
  RIO `0` as the host. Do not hardcode those numbers; they depend on how many
  phrase banks the mission loaded.
- If the load fails, `cop_siren_sample[city]` stays `-1` and `CarHasSiren()`
  falls back to sample `0` (the host city's siren). The warning says which
  city/bank failed. A wrong-city siren is used only as a last resort; a load
  failure never leaves the slot silent.
- `cop_siren_sample[]` is per level. `LoadLevelSFX()` resets every entry to
  `-1` before loading the host siren, so a previous level's appended bank or
  sample index cannot leak into the next.
- The bank is loaded from `SOUND\\VOICES2.BLK`; it does not depend on the
  source city's car-data buffer. The source city only has to be a D2 row and
  the slot's resident model has to be `0`.

## 5. Playback

### Player cars

`StartPlayerCarSounds(playerId, slot, pos)`:

- starts engine and idle on `playerId * 3` and `+1`;
- if `CarHasSiren(slot)` is non-zero, starts the siren on
  `playerId * 3 + 2` with the packed bank/sample;
- if it is zero, **stops** `playerId * 3 + 2` (a car change must not leave the
  previous car's siren looping).

`SoundTasks()` keeps the siren channel alive and controls its volume with
`horn.on`: a siren car uses the horn button as the siren switch; a civilian car
uses it as a normal horn. `DealWithHorn()` is suppressed for a siren car so a
stale horn request cannot start or stop the siren channel.

### AI cars

`DoDopplerSFX()` collects up to `MAX_SIREN_NOISES` (2) siren sources. Each
`siren_noise[]` entry remembers the packed sample it started with in
`siren_noise_sample[]`. Before the stop/start pass, if
`CarHasSiren(ap.model)` no longer matches that stored sample, the entry is
stopped and freed; the start loop then re-issues it with the new sample. This
is what makes a changed source city or a reused slot take effect.

A bank rebuild can move every appended car block, so
`RebuildImportedCarSoundBanks()` calls `ClearSirenNoises()`: all AI siren
entries are stopped and cleared, and `DoDopplerSFX()` rebuilds the qualifying
ones on the next frame. Player channels are re-issued by
`RestartPlayerCarSoundsForRebuild()`.

`RestartSirenForSlot(slot)` is the immediate version for a single mapping
change: it clears any AI entry whose car is on that slot and stops/restarts a
local player's siren channel. The per-frame comparison is the safety net when
the car's `ap.model` has not been re-pointed yet.

## 6. Horn versus siren

The third sample of `SOUND_BANK_CARS` has exactly one owner per car:

- **civilian:** it is the horn;
- **imported D1/custom cop:** it is the siren, and the normal horn paths are
  suppressed;
- **imported D2 cop:** the siren is in `SOUND_BANK_VOICES`, and the normal horn
  paths are also suppressed. Imported cops are siren-only: the car bank's third
  sample is suppressed, and the D2 siren lives in the voice bank rather than in
  the car bank.

Horn one-shots are guarded in `DealWithHorn()`, `LeadHorn()`, the `civ_ai.c`
lead-car horn, and the `cainescrossfire` Deadstar dash sound. Stock special
cars keep their existing `SOUND_BANK_SFX`/`SOUND_BANK_VOICES` siren samples.

## 7. Diagnostics and tests

`JERICHO_DIAG_SIREN=1` enables:

- `gamesnd: siren: slot N (CITY model M) sample S`
- `gamesnd: horn: slot N (CITY model M) sample S`

`gamesnd: <CITY> cop siren -> SOUND_BANK_VOICES sample N (bank M)` is always
logged when an imported D2 siren bank is appended.

`mp_carstress.py` sets the diagnostic env for each seat, reads the versioned
`JERICHO*.log` in each run directory (not only `JERICHO.log`), and fails on:

- `sound bank 3 is full` / `no car sound bank`;
- the same `(slot, city, model, sample)` appearing as both a horn and a siren;
- a slot appearing as both a civilian import and an imported-siren source.

The carstress rig's unrelated `not loaded` and coverage checks can still fail a
run; read the seat report to separate those from the sound contract.

## 8. Worked example

Host level: RIO (`GameLevel = 3`).

| Slot | Resident car | Source | `CarHasSiren()` |
| --- | --- | --- | --- |
| 3 | RIO model 0 (native cop) | `-1` | `(SOUND_BANK_VOICES, 0)` — host siren |
| 7 | CHICAGO model 0 | `0` | `(SOUND_BANK_VOICES, 18)` — CHICAGO bank 69 |
| 8 | HAVANA model 0 | `1` | `(SOUND_BANK_VOICES, 19)` — HAVANA bank 70 |
| 9 | VEGAS model 0 | `2` | `(SOUND_BANK_VOICES, 18)` — shares CHICAGO bank 69 |
| 10 | HAVANA model 3 (civilian) | `1` | `0` |
| 11 | level special car | varies | mission-specific `SOUND_BANK_SFX` or stock fallback |

The indices `18/19` are from one observed RIO-host run. The loader records
whatever the voice bank reports; the table is illustrative, not a constant.

## 9. Limits

- Only D2 source cities `0..3` have the stock siren banks used here. D1/custom
  cities use their baked embedded bank; a D1/custom import without one has no
  siren by design.
- `SOUND_BANK_VOICES` is a dynamic bank with a fixed sample capacity
  (`MAX_SOUND_BANK_SAMPLES`). A failed source-siren append logs a warning and
  falls back to the host sample.
- The source city's siren bank is shared by every car in that city. Two cars
  imported from the same D2 city use the same siren sample — that is the stock
  behaviour, not a bug.
- The host city's siren is sample 0 only because it is loaded first. Do not
  assume sample 0 is a non-host city's siren; use `cop_siren_sample[]`.
