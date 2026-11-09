# NatAmbio Alpha for Lyrion

Server-side stereo NAE and XTC processing for Lyrion Music Server 9 on Linux x86-64. The browser page recreates NatAmbio's Radar2 editor. Audio plays on the selected Lyrion player.

## Install

1. Open **Settings → Manage Plugins** and paste this URL into **Additional Repositories**.

   `https://github.com/DexiTheOne/NatAmbio-Lyrion/releases/latest/download/repo.xml`

2. Enable **NatAmbio Alpha**, apply, and restart Lyrion when prompted.
3. Open **NatAmbio Alpha** from the Lyrion web interface's plugins section. Select a player and turn on **Enable Routing**. Start a new track to apply the routing change.

The player needs to accept lossless FLAC, and the server needs Lyrion's bundled SoX or a compatible SoX on its path. Local FLAC, WAV, AIFF, MP3, and Ogg Vorbis can use the DSP route. Actual decoding support follows the server's SoX build. Keep the target player independent of sync groups for the initial release. For a source that still plays natively, check Lyrion's File Types settings and the player's supported formats. The dashboard shows whether the helper is rendering.

For SpotOn with Soloist, select **FLAC** as the streaming format in SpotOn's settings for the target player. Turn on **Enable Routing** for that player and start a new Spotify track.

## Controls

The dashboard fills the browser viewport and keeps the PCA field and technical readouts visible. The NAE and XTC sections each have a bypass switch, and the NAE section includes Preamp. Output Trim acts on the processed path and the master Bypass crossfades to delay-aligned dry audio. Window Blocks applies at the next stream because it changes latency.

**Reset DSP history on new track** starts each track with a cold DSP state. With the toggle off, the helper replays up to one second of the previous same-rate track into the new engine before emitting audio. The replay preserves block alignment and warms covariance and convolution history. Each track is independently latency-compensated and drained, so exact gapless parity with a single continuous VST render is outside this initial implementation. A seek, stop, or change of sample rate starts fresh.

CPU is the DSP thread's compute time as a share of the audio block budget, averaged over about one second. XRUN counts helper blocks whose elapsed time exceeded their budget. The peak readout follows the processed signal and turns red if the track clips. RESPONSE adjusts the PCA field while the speaker halos follow their own faster level envelope.

Changing Bypass, NAE Bypass, XTC Bypass, or another live DSP control reopens seekable NatAmbio streams at the current playhead on independent players. Playback briefly cuts while the player refills its buffer with freshly processed audio. On streams that cannot be reopened this way, changes take effect as the queued audio plays out.

Radar2 selects recorded DSP snapshots using the player's Lyrion playhead. The page shows the estimated render lead and indicates when live settings are queued behind buffered audio. Once the player reaches the revised audio, the status shows the track position where the settings applied. Exact display alignment depends on the timing reported by the player.

## Build and package

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 tools/package.py build/natambio-alpha https://example.org/releases/download/v0.1.11
```

The package script writes a versioned ZIP and Lyrion repository XML with its SHA-1 checksum. Tagging a commit with the version in `plugin/install.xml` builds and publishes both release assets on GitHub.

## Provenance

The JUCE-free core comes from the [NatAmbio VST3 project](https://github.com/digitalfrost84/NatAmbio-VST3), based on commit `9c01017` with the local DSP changes present when this port started. NatAmbio's NAE and XTC algorithms originate in [Raul Fernández Ortega's NatAmbio](https://github.com/raul-fernandez-ortega/natambio), pinned in the VST project at `6723da8807807aacc71b5d60610dc4f6b808a120`. See [LICENSE](LICENSE) and the font licenses shipped with the web page.
