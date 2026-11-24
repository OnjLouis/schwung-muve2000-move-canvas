"""Create the two Schwung manifests and editors from the desktop catalogue."""
import argparse
import json
from pathlib import Path
import re
import shutil

here = Path(__file__).resolve().parent
source = here.parent / "Source"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("dsp", type=Path)
args = parser.parse_args()

controls = []
for line in (source / "Controls.h").read_text().splitlines():
    if 'ControlScope::' not in line or not line.lstrip().startswith('{"'):
        continue
    key, name, low, high, default = re.match(
        r'\s*\{"([^"]+)","([^"]+)",(\d+),(\d+),(\d+),', line).groups()
    unit = re.search(r'ControlUnit::(\w+)', line)
    controls.append([key, name, int(low), int(high), int(default), unit[1] if unit else 'Raw'])
programs = []
for line in (source / "ProgramData.h").read_text().splitlines():
    if not line.lstrip().startswith('{"'):
        continue
    _, name, map_id, bank, pc, drum = json.loads('[' + line.strip().rstrip(',')[1:-1] + ']')
    programs.append([name, map_id, bank, pc, drum])
types = re.findall(r'\{"([^"]+)",0x[0-9a-fA-F]+,0x[0-9a-fA-F]+\}', (source / 'Insertion.h').read_text())
assert len(controls) == 93 and len(programs) == 2958 and len(types) == 65
pages = [
    ("Part and program", [0, 1, 2, 3, 15]), ("Part mixer", [4, 5, 6, 7, 8, 9, 10]),
    ("Tone and envelope", [11, 12, 13, 14, 16, 17, 18, 19]),
    ("Tuning and sends", [20, 21, 22, 23]), ("Master", [24, 25, 26]),
    ("Reverb", range(27,34)), ("Chorus", range(34,43)),
    ("Delay", range(43,54)), ("Equalizer", range(54,58)),
    ("Drum note", range(58,66)), ("Insertion effects", range(66,73)),
    ("Effect parameters 1 to 8", range(73,81)), ("Effect parameters 9 to 16", range(81,89)),
    ("Effect parameters 17 to 20", range(89,93))]
pages = [[title,list(indices)] for title, indices in pages]
assert sorted(index for _, indices in pages for index in indices) == list(range(len(controls)))
main_knobs = [4,5,11,12,13,14,8,9]
for tool in (False, True):
    name = "Move Canvas MIDI Player" if tool else "Move Canvas"
    ident = "sound-canvas-midi-player" if tool else "sound-canvas-bridge"
    target = args.output / ("tools" if tool else "sound_generators") / ident
    target.mkdir(parents=True, exist_ok=True)
    caps = dict(audio_out=True, midi_in=True)
    manifest = dict(id=ident, name=name, abbrev="MC MIDI" if tool else "MCanvas", version="0.1.3",
                    author="OnjMusic", ui="ui.js", dsp="dsp.so", api_version=2, capabilities=caps)
    if tool:
        manifest.update(component_type="tool", tool_config=dict(interactive=True, skip_file_browser=True))
        caps.update(suspend_keeps_js=True, suspend_self_managed=True)
    else:
        manifest.update(ui_chain="ui.js")
        caps.update(midi_channels=16, chainable=True, component_type="sound_generator", requires_continuous_processing=True)
        order = main_knobs + [i for i in range(len(controls)) if i not in main_knobs]
        caps["chain_params"] = [dict(key=k,name=n,type="int",min=lo,max=hi,default=1 if k=="routing" else d,step=1)
                                for i in order for k,n,lo,hi,d,_ in [controls[i]]]
    (target / "module.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf8")
    metadata = dict(tool=tool, controls=controls, programs=programs, types=types, pages=pages, knobs=main_knobs)
    js = (here / "editor.js").read_text(encoding="utf8").replace("/* MODULE_DATA */",json.dumps(metadata,separators=(',',':')))
    (target / "ui.js").write_text(js, encoding="utf8")
    shutil.copy2(args.dsp,target/"dsp.so")
    help_folder = "player" if tool else "synth"
    shutil.copyfile(here / help_folder / "help.json", target / "help.json")
