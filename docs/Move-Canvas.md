# Playing Move Canvas

## Choose a sound

Open **Preset bank** to go straight to the current sound on the current map. Opening the list preserves your patch and its edits. Turn Jog while playing to audition neighbouring presets. Click the sound to return to the menu. When you reopen the bank, focus returns to the playing preset.

For a shorter list, open **Instrument families** or **Drum kits** from the main menu. Opening a family or the kit list preserves the playing sound until you turn to a sound or click one. The **Back** row returns to the parent menu. To switch between SC-55, SC-88, SC-88 Pro and SC-8820, use **Part and program → Sound Map**. The full catalogue has 2,958 entries across those four maps.

**Part and program → Edit part** defaults to **Follow track**. A slot receiving channel 3 edits part 3 as soon as you open the editor. A fixed forward channel takes priority. On an Omni slot without a fixed forward channel, play a note to select the part. You can also choose a fixed part yourself.

**To play drums on channel 3**, open **Drum kits** and choose a kit. The generator selects part 10 for the kit. New instances use **Selected part** keyboard routing, so the same pads can play drums. Browsing a melodic sound returns to the track part. For an older saved instance, choose **Part and program → Keyboard Routing → Selected part** to get this behaviour. **Multitimbral** keeps the incoming channels separate and uses channel 10 for drums.

## Edit the sound

Turn **Jog** to an edit page and click to open it. Touch an encoder to hear its parameter and value. Turn it to change that value. You can also jog to a parameter, click to edit, turn Jog, then click to finish. **Menu** moves back one level.

The main knobs control volume, pan, brightness, resonance, attack, release, reverb send and chorus send. Other pages cover tuning, envelopes, master controls, reverb, chorus, delay, equalizer, individual drum notes and insertion effects. The effect pages expose all 20 insertion parameters. Use **All notes off** if a note hangs.

**Part and program → Pad response** starts on **Fast** for live playing. Choose **Buffered** for more headroom if you hear crackling on a busy patch. The generator remembers this setting with the set. The MIDI file player uses its own playback buffer.

## Play a MIDI file

Unload the Move Canvas generator before opening **Move Canvas MIDI Player** in **Tools**. Both use the same engine slot.

1. Choose **Open MIDI file**, browse the User Library with Jog and click the file.
2. Leave **Playback mode** on **Render First** for dense arrangements or files using multiple ports. Press **Play**. **Shift + Click** reads the status and render progress. The player prepares up to four MIDI ports separately, then mixes them. It follows the file's tempo and supports files up to ten minutes. Press Play during preparation to cancel.
3. During playback, press **Play** to pause or resume. **Loop** toggles repeat. **Shift + Play** returns to the start. **Shift + Jog** on the main menu seeks in five-second steps.

Choose **Live** for a single MIDI port when you want to change the mix, play external MIDI or use **Follow Move transport**. With Follow enabled, Move's transport and tempo drive playback. From the player's main menu, the lower 16 pads mute parts. Hold Shift and press a pad to solo it. Turn knobs to trim parts 1–8, or hold Shift for parts 9–16. **Lock selected part patch** keeps file bank and program messages from replacing your choice for that part.

**Back** hides the player. **Shift + Back** exits it so the generator can use the engine again. If Live produces gaps, stop, choose Render First and play again.
