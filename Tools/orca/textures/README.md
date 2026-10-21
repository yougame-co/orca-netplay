# Orca's texture pack

`Data/Sys/Orca/Textures/RSBE01/` holds the labels of the Online menu (ORCA.md "Online menu"):
Dolphin hires textures that Orca loads in every session, so the game's own buttons read PLAY
ONLINE, CASUAL and RANKED and Brawl's page title reads ONLINE. Brawl and Project+ both load their
textures as `RSBE01`; their labels hash differently, so one folder serves both.

| File (and its `_mip1`, `_mip2`)      | Replaces                                                                                        | Game     |
| ------------------------------------ | ----------------------------------------------------------------------------------------------- | -------- |
| `tex1_127x96_fdd5334e5af7357c_2.png` | the main menu's "NINTENDO Wi-Fi CONNECTION" button label (MenMainFrTop15)                       | Brawl    |
| `tex1_120x16_a56825159146a723_0.png` | the page title "NINTENDO WFC" (MenMainTtlJ.03; it heads the ONLINE pages and their breadcrumbs) | Brawl    |
| `tex1_144x88_115d82941c7412cd_2.png` | With Anyone's "BASIC BRAWL" (MenMainWifi22): CASUAL                                             | Brawl    |
| `tex1_160x80_e364b4db513567ba_2.png` | With Anyone's "TEAM BATTLE" (MenMainWifi23): RANKED                                             | Brawl    |
| `tex1_144x88_ecd09d11be34045f_5.png` | With Anyone's "BASIC VERSUS" (MenMainWifi22): CASUAL                                            | Project+ |
| `tex1_160x80_5b8f52ac0127b2a8_5.png` | With Anyone's "TEAM BATTLE" (MenMainWifi23): RANKED                                             | Project+ |
| `tex1_132x48_44a1dd712ec999f0_159b390ed5eec04d_9.png` | the character select's BRAWL logo (MenSelchrPanelRule3.1) marked casual: CASUAL | Brawl |
| `tex1_132x48_44a1dd712ec999f0_4d236969243ec2d8_9.png` | the same marked ranked: RANKED | Brawl |
| `tex1_132x48_44a1dd712ec999f0_952be74b413d03a4_9.png` | the same marked friends: FRIENDS | Brawl |
| `tex1_128x56_57564f7b1ba3bc1c_96a987e9e0ec657e_9.png` | the character select's VERSUS (MenSelchrPanelRule3.1) marked casual: CASUAL | Project+ |
| `tex1_128x56_57564f7b1ba3bc1c_e2dffeab0d95d424_9.png` | the same marked ranked: RANKED | Project+ |
| `tex1_128x56_57564f7b1ba3bc1c_faaf2c3553d21273_9.png` | the same marked friends: FRIENDS | Project+ |

Project+'s own button already reads PLAY ONLINE and its title ONLINE.

The relabels (`Core/Orca/UX/Relabel.h`, ORCA.md "On-screen UI") add 38 more names, listed with
their words in `RELABELS` in the script. They replace READY TO FIGHT! (with PRESS START TO LOCK IN!
and similar), the STAGE SELECT title (with each strike or pick step, right-aligned so the overlay's
timer follows), the stage select's BRAWL / VERSUS (RANKED, CASUAL) and Project+'s stage select
legend (STRIKE, PROPOSE, TAKE BACK; X, A, B).

The character select's titles are paletted (C8), so their names include the palette's hash. Orca
never draws into the game's texture. Instead, in an online room it nudges the blue of the texture's
transparent background colour by one to three steps, one per mode (`Core/Orca/UX/CssTitle.h`), so
each mode's palette hashes to a different name above. The names use Dolphin's own texture hash.

The art is original, drawn by `make-labels.py` (needs Pillow) in each game's style with
open-licence fonts. Nothing comes from the game's art; the script only uses the positions of the
game's letters so ours sit in the same place. Each label is 4x the game's size, with mip levels down
to 1x (`_mip1.png` at 2x, `_mip2.png` at 1x). Dolphin samples a custom texture's mips even when the
game's texture has none, so at 1x internal resolution thin strokes stay clean.
Regenerate with:

```bash
python3 Tools/orca/textures/make-labels.py            # writes Data/Sys/Orca/Textures/RSBE01
python3 Tools/orca/textures/make-labels.py --out /tmp/x --sheet /tmp/sheet.png   # a preview
```

The output is byte-identical for the same Pillow (11.3.0 made the committed files).

## Fonts

The fonts are from [google/fonts](https://github.com/google/fonts) and are licensed under the
SIL Open Font License, Version 1.1. Their licence texts, with the copyright notices, are next to
them in `fonts/`. None is shipped with Orca: only the PNGs drawn with them are.

| Font                             | Use                              | Licence                                                                         | Source              | SHA-256                                                            |
| -------------------------------- | -------------------------------- | ------------------------------------------------------------------------------- | ------------------- | ------------------------------------------------------------------ |
| `fonts/ArchivoBlack-Regular.ttf` | Brawl's labels                   | `fonts/OFL-ArchivoBlack.txt` (Copyright 2017 The Archivo Black Project Authors) | `ofl/archivoblack/` | `dd9a89a019b4849f66ab75455fe7bdf931311042cbb0f0f97acc061539703180` |
| `fonts/Anton-Regular.ttf`        | Project+'s labels, Brawl's title | `fonts/OFL-Anton.txt` (Copyright 2020 The Anton Project Authors)                | `ofl/anton/`        | `a4ba3a92350ebb031da0cb47630ac49eb265082ca1bc0450442f4a83ab947cab` |
| `fonts/RussoOne-Regular.ttf`     | READY TO FIGHT!'s relabels       | `fonts/OFL-RussoOne.txt` (Copyright 2011-2012 Jovanny Lemonad)                  | `ofl/russoone/`     | `bc0abcc660bd8b7ad3000ecb2898a27c58a29a50f7ec81652fa12e75148d09df` |
| `fonts/Cinzel[wght].ttf`         | Brawl's stage select title       | `fonts/OFL-Cinzel.txt` (Copyright 2020 The Cinzel Project Authors)              | `ofl/cinzel/`       | `f4d83d34d1f6c741193e4acf4b3dff9531e5a67b6aa65228d00a7db72a4e0f34` |

## Rules

- Replacing a texture changes only what is drawn: the texture cache hashes the game's texture in
  RAM and swaps the host GPU texture; nothing Orca draws reaches emulated memory (Profile.cpp
  forces EFB copies to texture only, EFB access and bounding box off). The pack is therefore not
  in the compatibility key. `YG_HASHLOG` is identical with and without it in both games.
- In a session Orca's textures win over a player's own pack in `<user>/Load/Textures` for the
  names Orca replaces; the player's pack still supplies every other name
  (`CollectHiresTextureFiles` in `VideoCommon/HiresTextures.cpp`). Orca's pack is always
  preloaded and is not loaded outside a session.
- A new label: add an entry to `LABELS` with the texture's Dolphin name (dump it with
  `-C Graphics.Settings.DumpTextures=True`, then delete the dump: it is the game's art), run the
  script, and the unit test `OrcaOnlineMenuTextures` checks the name, the size and the mip levels.
