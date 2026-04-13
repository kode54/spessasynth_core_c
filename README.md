<!--suppress HtmlDeprecatedAttribute, HtmlRequiredAltAttribute, HtmlExtraClosingTag -->
<p align='center'>
<img src='https://raw.githubusercontent.com/kode54/spessasynth_core_c/refs/heads/main/website/spessasynth_logo_rounded.png' width='300' alt='SpessaSynth logo'>
</p>

_A powerful multipurpose SF2/DLS/MIDI library, now ported to C, through the magic of vibes and lots of manual labor as well. It should be fairly compatible with any C11 compiler, but it may work with something older as well.

It allows you to:

- Play MIDI files using SF2/SF3/DLS files!
- Read MIDI files!
- Read SF2/SF3 files!
- Read DLS files to a SF2 compatible structure!
- [and more!](#current-features)

### v4.2.0 The Effects Update is here!

Featuring Reverb, Chorus, Delay, Insertion effects and more!

**[Project site (consider giving it a star!)](https://github.com/kode54/spessasynth_core_c)**

**[Original Upstream Project site (also consider giving it a star!)](https://github.com/spessasus/spessasynth_core)**

### Made with spessasynth_core_c

- [Cog, an audio file player for macOS](https://cog.losno.co)

### Documentation

- More will be forthcoming, as I craft up documentation for this.

**SpessaSynth C Project index**

- [spessasynth_core_c](https://github.com/kode54/spessasynth_core_c) (you are here)

**SpessaSynth Project index**

- [spessasynth_core_c](https://github.com/spessasus/spessasynth_core) (you are here) - SF2/DLS/MIDI library
- [spessasynth_lib](https://github.com/spessasus/spessasynth_lib) - spessasynth_core wrapper optimized for browsers and
  WebAudioAPI
- [SpessaSynth](https://github.com/spessasus/SpessaSynth) - online/local MIDI player/editor application
- [SpessaFont](https://github.com/spessasus/SpessaFont) - online SF2/DLS editor

## Current Features

### Easy Integration

- **Modular design:** _Easy integration into other projects (only load what you need)_
- **Flexible:** _It's not just a MIDI player!_ Well, not totally.
- **Easy to Use:** _Basic setup is
  just [a few lines of code!](https://github.com/losnoco/Cog/blob/main/Plugins/MIDI/MIDI/SpessaPlayer.mm)_
- **Minimal and only optional dependencies:** _Batteries included!_ Except for STB Vorbis and libFLAC, if you want those!

### Powerful MIDI Synthesizer

- Suitable for both **real-time** and **offline** synthesis
- **Excellent SoundFont support:**
    - **Full Generator Support**
    - **Full Modulator Support:** _Comparable to BASSMIDI now!_
    - **GeneralUser-GS Compatible:**
      _[See more here!](https://github.com/mrbumpy409/GeneralUser-GS/blob/main/documentation/README.md)_
    - **SoundFont3 Support:** Play compressed SoundFonts!
    - **Experimental SF2Pack Support:** Play soundfonts compressed with BASSMIDI! (_Note: only works with vorbis or flac
      compression at this time_)
- **Great DLS Support:**
    - **DLS Level 1 Support**
    - **DLS Level 2 Support**
    - **Mobile DLS Support**
    - **Correct articulator support:** _Converts articulators to both modulators and generators!_
    - **Tested and working with gm.dls!**
    - **Correct volume:** _Properly translated to SoundFont volume!_
    - **A-Law encoding support**
    - **Both unsigned 8-bit and signed 16-bit sample support (24-bit could easily be supported, theoretically, if it is even in use anywhere)**
    - **Detects special articulator combinations:** _Such as vibratoLfoToPitch_
- **Sound bank manager:** Stack multiple sound banks!
- **Unlimited channel count:** Your CPU is the limit! Though technically, it limits to whatever you set it to on startup.
- **Built-in, configurable effects:**
    - **Reverb:** _Multiple characters including delay and panning delay!_
    - **Chorus:** _Modulated delay lines with multiple presets!_
    - **Delay:** _Three delay lines for all of your delay needs!_
    - **Insertion Effects:** _The ultimate effects, they can give your sounds a completely different character! (limited support)_
    - **GS Compatible:** _MIDI files can configure the effects accurately!_
    - **Replaceable:** _Effects not to your liking? You can replace them with your own!_
- **Excellent MIDI Standards Support:**
    - **MIDI Controller Support:** Default supported
      controllers [here](https://spessasus.github.io/spessasynth_core/extra/midi-implementation#default-supported-controllers)
    - **Portamento Support:** _Smooth note gliding!_
    - **Sound Controllers:** _Real-time filter and envelope control!_
    - **MIDI Tuning Standard Support:**
      _[more info here](https://spessasus.github.io/spessasynth_core/extra/midi-implementation#midi-tuning-standard)_
    - [Full **RPN** and extensive **NRPN**
      support](https://spessasus.github.io/spessasynth_core/extra/midi-implementation#supported-registered-parameters)
    - **SoundFont2 NRPN Support**
    - [**AWE32**
      NRPN Compatibility Layer](https://spessasus.github.io/spessasynth_core/extra/midi-implementation#awe32-nrpn-compatibility-layer)
    - [**Roland GS** and **Yamaha XG**
      support!](https://spessasus.github.io/spessasynth_core/extra/midi-implementation#supported-system-exclusives)

### Powerful and Fast MIDI Sequencer

- **Supports MIDI formats 0, 1, and 2:** _note: format 2 support is experimental as it's very, very rare._
- **[Multi-Port MIDI](https://spessasus.github.io/spessasynth_core/extra/about-multi-port) support:** _More than 16
  channels!_
- **Smart preloading:** Only preloads the samples used in the MIDI file for smooth playback _(down to key and
  velocity!)_ Though currently, bank support loads the entire bank to memory at once, this is planned for changing.
- **Lyrics support:** _Add karaoke to your program!_ Maybe the port includes this.
- **Raw lyrics available:** Decode in any encoding! _(Kanji? No problem!)_
- **Loop points support:** _Ensures seamless loops!_ Should still work!

### Read (but not yet Write) SoundFont and MIDI Files with Ease

#### Read (and eventually write) MIDI files

- **Smart name detection:** _Handles incorrectly formatted and non-standard track names!_
- **Raw name available:** Decode in any encoding! _(Kanji? No problem!)_
- **Port detection during load time:** _Manage ports and channels easily!_
- **Used channels on track:** _Quickly determine which channels are used!_
- **Key range detection:** _Detect the key range of the MIDI!_
- **Easy MIDI editing:**
  Use [helper functions](https://spessasus.github.io/spessasynth_core/writing-files/midi#modifymidi) to modify the
  song to your needs!
- **Loop detection:** _Automatically detects loops in MIDIs (e.g., from **Touhou Project**)_
- **First note detection:** _Skip unnecessary silence at the start by jumping to the first note!_
- **Lyrics support:** _Both regular MIDI and .kar files!_
- ~~[Write MIDI files from scratch](https://spessasus.github.io/spessasynth_core/midi/creating-midi-files)~~
- ~~Easy saving~~: ~~Save with~~
  ~~just [one function!](https://spessasus.github.io/spessasynth_core/writing-files/midi#writemidi)~~

#### Read (and eventually maybe write) [RMID files with embedded sound banks](https://github.com/spessasus/sf2-rmidi-specification#readme)

- **[Level 4](https://github.com/spessasus/sf2-rmidi-specification#level-4) compliance:** Reads and writes _everything!_
- ~~Compression and trimming support: Reduce a MIDI file with a 1GB sound bank to as small as 5MB!~~
- **DLS Version support:** _The original legacy format with bank offset detection!_
- **Automatic bank shifting and validation:** Every sound bank _just works!_
- **Metadata support:** Add title, artist, album name and cover and more! And of course, read them too! _(In any
  encoding!)_
- **Compatible with [Falcosoft Midi Player 6!](https://falcosoft.hu/softwares.html#midiplayer)**

#### Read (and eventually write) SoundFont2 files

- **Easy info access:** _Just
  a [struct with nested string and data pointers](https://github.com/kode54/spessasynth_core_c/tree/main/spessasynth_core/include/spessasynth)_
- **Smart trimming:** Trim the sound bank to only include samples used in the MIDI _(down to key and velocity!)_ _Coming soonish!_
- **SF3 conversion:** _Compress SoundFont2 files to SoundFont3 with variable quality!_ _Also planned!_

#### Read (and eventually write) SoundFont3 files

- Same features as SoundFont2 but with now with **Ogg Vorbis compression!**
- **Variable compression quality:** _You choose between file size and quality!_
- **Compression preserving:** _Avoid decompressing and recompressing uncompressed samples for minimal quality loss!_
- **Custom compression function:** _Want a different format than Vorbis? No problem!_

#### Read (and eventually write) DLS Level One or Two files

- Read DLS (DownLoadable Sounds) files like SF2 files!
- Converts articulators to both **modulators** and **generators**!
- Works with both unsigned 8-bit samples and signed 16-bit samples!
- ~~A-Law encoding support: Sure, why not?~~
- **Covers special generator cases:** _such as modLfoToPitch_!
- **Correct volume:** _looking at you, Viena and gm.sf2!_
- Support built right into the synthesizer!

### Export MIDI as WAV

- Up to someone to implement a tool for this, but a WAV writer is in the library, and an example player is provided.

### Limitations

- Audio engine was originally written in pure TypeScript, but has been meticulously ported to C by the power of starting with vibes, working hard when that failed, and using a little vibe time for ages to find the dumbest of missing features compared to the original implementation.
- [SF2 to DLS Conversion limits](https://spessasus.github.io/spessasynth_core/extra/dls-conversion-problem) _Coming soon!_

#### TODO

- Improve the performance of the engine?

### Special Thanks

- [The original SpessaSynth](https://github.com/spessasus/SpessaSynth)
- [Claude Sonnet 4.6](https://claude.ai) - Used to bootstrap the conversion process, and also to help find where I was foolish and missed something important.
- [FluidSynth](https://github.com/FluidSynth/fluidsynth) - for the source code that helped implement functionality and
  fixes
- [Polyphone](https://www.polyphone-soundfonts.com/) - for the soundfont testing and editing tool
- [Meltysynth](https://github.com/sinshu/meltysynth) - for the initial low-pass filter implementation
- [RecordingBlogs](https://www.recordingblogs.com/) - for detailed explanations on MIDI messages
- [stbvorbis.js](https://github.com/hajimehoshi/stbvorbis.js) - for the Vorbis decoder
- [fflate](https://github.com/101arrowz/fflate) - for the MIT DEFLATE implementation
- [tsup](https://github.com/egoist/tsup) - for the TypeScript bundler
- [foo_midi](https://github.com/stuerp/foo_midi) - for useful resources on XMF file format
- [Falcosoft](https://falcosoft.hu) - for help with the RMIDI format
- [Christian Collins](https://schristiancollins.com) - for various bug reports regarding the synthesizer
- **And You!** - for checking out this project. I hope you like it :)

**If you like this project, consider giving it a star. It really helps out!**

### Short example: MIDI player

- See: [example.c](https://github.com/kode54/spessasynth_core_c/blob/main/spessasynth_core/examples/example.c)

Read more in the [original project's documentation](https://spessasus.github.io/spessasynth_core) or wait for me to make some documentation for this!

### Building

Use CMake with the included project file.

## Use in other projects

Note that I do not endorse the use of this project in anything related to foobar2000, but the license does not forbid anyone from actually using it for that purpose, since it's a mostly free world, after all. Your mileage may vary.

## License

Copyright © 2026 Christopher Snowhill
Copyright © 2026 Spessasus
Licensed under the Apache-2.0 License.

#### Legal

This project is in no way endorsed or otherwise affiliated with the MIDI Manufacturers Association,
Roland Corporation, Yamaha Corporation, Creative Technology Ltd. or E-mu Systems, Inc.,
or any other organization mentioned.
SoundFont® is a registered trademark of Creative Technology Ltd.
All other trademarks are the property of their respective owners.
