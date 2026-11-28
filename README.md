# DreamSFX

**Procedural sound for Unreal Engine 5.8, written in plain text.**

A `.dsfx` file can describe a two-note menu chime, a piano and music-box duet, or a spell that swells as a player holds a button. Write the notes, signal paths and rules in a text editor, audition them in Unreal, then build a stereo MetaSound Source. A Blueprint can play it and drive the inputs you named in the file.

[简体中文](README.zh-CN.md) · [83 factory presets](Resources/Presets/) · [DSL reference](https://github.com/kisspread/SoundLab/blob/main/Docs/TextFormat.md) · [SoundLab demo project](https://github.com/kisspread/SoundLab)

## What a sound looks like

[Listen to the menu chime · WAV](Docs/Audio/soft-confirm.wav) · [Open its source](Resources/Presets/UI/sine-confirm-soft.dsfx)

```text
sound benchmark.sine_confirm_soft
preview .125 s
tail .09 s
input transposeBy range -12 12 default 0
patch bell { tone=voice.sine(gainDb=-26,voiceLimit=4) }
track chime uses bell {
  tempo 180; velocity .7; gate .7; transpose transposeBy
  C5/32 G5/16
}
output chime.tone
```

The chime plays C5 then G5 on a sine voice. Change `G5` to `E5` for a smaller upward step. To keep the interval and shift both notes, set `transposeBy` on the Audio Component. The same named input can be saved in a text instance for another menu.

## Quick start

1. Clone the demo project with its two plugins, build the **SoundLabEditor** target for UE 5.8, then open `SoundLab.uproject`.

   ```sh
   git clone --recurse-submodules https://github.com/kisspread/SoundLab.git
   ```

   In an existing UE 5.8 project, place **DreamSFX** and **EightBitMetaSound** side by side under `Plugins/`, enable DreamSFX and MetaSound, and build the project's Editor target. EightBitMetaSound supplies the retro synth. [EightBitMetaSound repository](https://github.com/kisspread/EightBitMetaSound)

2. Open **Window → DreamSFX Sounds → Factory presets**. Under **UI**, double-click the confirmation sound to hear it and see its waveform. **Preview volume** adjusts your listening level.
3. For a lower variation, select the sound, enter an instance name and click **Create project instance**. Enable the `transposeBy` override, set it to `-5`, then click **Save instance**. To write a new sound, put a UTF-8 `.dsfx` file with its own sound ID under the project's `DSFX/` directory and select **Project sounds**.
4. Click **Build MetaSound asset**. The Source appears under `Content/DreamSFX/Graphs/`. Assign it to an Audio Component and call **Play**. Its canvas shows one **DreamSFX Graph Player** connected to the exposed controls, stereo outputs and completion trigger. Edit the instruments and signal paths in the source text.

## Sounds to try

| Sound | What to listen for | Source |
| --- | --- | --- |
| Retro square wave | `dutyCycle` changes the width of each pulse and the timbre of the note. | [Retro square](Resources/Presets/Retro/retro-square.dsfx) |
| Piano and music box | Both tracks play the same phrase. The piano enters half a second later, seven semitones lower, in a 3.2-second hall. The music box has its own 1.2-second room. | [Lantern duet](Resources/Presets/MusicBox/lantern-duet.dsfx) |
| Day into dusk | Move `worldZ` to crossfade the two pads, or change `airSpeed` to hear the flight wind build. The wind preset is a two-line instance that solos one layer. | [Tour](Resources/Presets/Duskward/Scenes/tour.dsfx) · [Wind instance](Resources/Presets/Duskward/Layers/wind.dsfx) |
| Charged spell | Follow the runes through the fall, shock wave, impact and ringing ears. The 19-second audition drives the same inputs that gameplay can supply. | [Full spell](Resources/Presets/Magic/blastFull.dsfx) |

## Authoring with AI and the command line

Give an AI a sound brief and the nearest factory preset. For this chime, try “keep the first note, lower the second to E5, and leave the volume alone.” Review the change in the `.dsfx` diff. The `DreamSFXGraphAssets` commandlet can compile the result, render a listening WAV and return diagnostics with a file, code and, when available, line and column.

From the SoundLab root in PowerShell, set `UE_ROOT` to your engine installation directory (the directory containing `Engine/`). This command validates and renders the example above.

```powershell
$editor = Join-Path $env:UE_ROOT 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$project = (Resolve-Path './SoundLab.uproject').Path
$presets = (Resolve-Path './Plugins/DreamSFX/Resources/Presets').Path
$out = Join-Path (Get-Location) 'Saved/DreamSFX/ReadmeAudition'
& $editor $project -run=DreamSFXGraphAssets "-SourceDir=$presets" `
    '-Id=benchmark.sine_confirm_soft' -ValidateOnly "-RenderDir=$out" `
    "-Report=$out/report.json" -unattended -nullrhi -nosound -nop4
```

Open the WAV in `Saved/DreamSFX/ReadmeAudition/`. The JSON report includes the input ranges, duration, peak, RMS and clipped-sample count. The audition is rendered by DreamSFXCore at 48 kHz in stereo. Omit `-ValidateOnly` to also write the MetaSound asset, or omit `-Id` to process the whole source directory.

## Inside the plugin

| Module | Role |
| --- | --- |
| [DreamSFXCore](Source/DreamSFXCore/) | Compiles the DSL, schedules notes and rules, and renders stereo audio. |
| [DreamSFXRuntime](Source/DreamSFXRuntime/) | Stores the sound program inside the generated asset and loads it for playback. |
| [DreamSFXMetaSound](Source/DreamSFXMetaSound/) | Implements Graph Player and connects UE triggers and parameters to the running sound. |
| [DreamSFXEditor](Source/DreamSFXEditor/) | Provides the preset browser, waveform, instance controls, asset builder and batch commandlet. |

DreamSFX **2.0.0 beta** has been built and tested on **Win64 with UE 5.8**. The SoundLab repository hosts the authoring guide and language reference, both in Chinese. [Authoring guide](https://github.com/kisspread/SoundLab/blob/main/Docs/DreamSFX-QuickStart.md) · [Language and nodes](https://github.com/kisspread/SoundLab/blob/main/Docs/TextFormat.md)
