import { decodeDelta, decodeAcceleratedDelta, isCapacitiveTouchMessage } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

const fields = [['program', 'Patch', 127], ['part', 'Part', 63], ['bank', 'Bank MSB', 127], ['lsb', 'Bank LSB', 127], ['octave', 'Octave', 4, -4]];
const knobs = [['volume', 'Volume'], ['pan', 'Pan'], ['cutoff', 'Cutoff'], ['resonance', 'Resonance'],
    ['attack', 'Attack'], ['release', 'Release'], ['reverb', 'Reverb Send'], ['chorus', 'Chorus Send']];
const knobCC = 71;
const groups = [['Instruments', 0], ['Drums', 127], ['SFX Kits', 126], ['Bank Variations'], ['Settings']];
let browser = '', group = 0;
let selected = 0, editing = false, previous = '', redraw = true, lastKnob = -1, lastTurn = 0;
function value(field) {
    const n = Number(host_module_get_param(field[0]));
    return Number.isFinite(n) ? n : 0;
}
function display(field) {
    const n = value(field);
    if (field[0] === 'program') return patchLabel();
    if (field[0] === 'part') return String.fromCharCode(65 + Math.floor(n / 16)) + (n % 16 + 1);
    return String(n);
}
function patchLabel() { return String(host_module_get_param('patch_name') || 'Program ' + (value(fields[0]) + 1)); }
function speak() {
    const field = fields[selected], n = value(field);
    const text = field[0] === 'octave' && n !== 0 ? (n < 0 ? 'minus ' : 'plus ') + Math.abs(n) : display(field);
    announce(field[1] + ', ' + text);
}
function init() { selected = 0; editing = false; browser = ''; group = 0; previous = ''; redraw = true; lastKnob = -1; lastTurn = 0; }
function tick() {
    const status = String(host_module_get_param('status') || 'Starting Muve2000.').replace('Starting MU2000.', 'Starting Muve2000.');
    if (status !== previous) { previous = status; announce(status); redraw = true; }
    if (!redraw) return;
    redraw = false;
    clear_screen();
    drawMenuHeader('Muve2000', editing ? 'EDIT' : '');
    if (status !== 'Ready.') {
        print(2, 20, status.slice(0, 21), 1);
        print(2, 31, status.slice(21, 42), 1);
        if (typeof host_flush_display === 'function') host_flush_display();
        return;
    }
    const isGroups = browser === 'groups';
    drawMenuList({
        items: isGroups ? groups : fields, selectedIndex: isGroups ? group : selected,
        listArea: { topY: menuLayoutDefaults.listTopY, bottomY: menuLayoutDefaults.listBottomWithFooter },
        getLabel: field => field[isGroups ? 0 : 1], getValue: isGroups ? () => '' : display, valueAlignRight: true, announce: false
    });
    drawMenuFooter(isGroups ? 'Jog:select Click:open' : browser === 'variation' ? 'Jog:bank Click:done'
        : editing ? 'Jog:change Click:done' : selected ? 'Jog:select Click:edit' : 'Jog:patch Click:browse');
    if (typeof host_flush_display === 'function') host_flush_display();
}
function onMidiMessageInternal(data) {
    if (!data || data.length < 3 || previous !== 'Ready.') return;
    const status = data[0] & 240, cc = data[1], v = data[2];
    const touch = status === 144 && v > 0 && cc < knobs.length;
    const turn = status === 176 && cc >= knobCC && cc < knobCC + knobs.length;
    if (touch || turn) {
        const index = touch ? cc : cc - knobCC;
        const knob = knobs[index], d = turn ? decodeAcceleratedDelta(v, cc) : 0;
        if (turn && !d) return;
        if (turn) host_module_set_param('step:' + knob[0], String(d));
        const now = Date.now();
        const name = touch || index !== lastKnob || now - lastTurn > 1500 ? knob[1] + ', ' : '';
        announce(name + value(knob));
        lastKnob = index;
        lastTurn = now;
        return;
    }
    if (isCapacitiveTouchMessage(data) || status !== 176) return;
    if (browser) {
        if ((cc === 50 || cc === 3) && v) {
            if (cc === 50 || browser === 'variation') {
                browser = '';
                announce(patchLabel());
            } else {
                const [label, bank] = groups[group];
                if (label === 'Settings') { browser = ''; selected = 1; speak(); }
                else if (label === 'Bank Variations') {
                    browser = 'variation';
                    announce(label + ', ' + patchLabel());
                } else {
                    host_module_set_param('group', String(bank));
                    browser = '';
                    announce(label + ', ' + patchLabel());
                }
            }
            redraw = true;
        } else if (cc === 14) {
            const d = decodeDelta(v);
            if (!d) return;
            if (browser === 'groups') {
                group = ((group + d) % groups.length + groups.length) % groups.length;
                announce(groups[group][0]);
            } else {
                host_module_set_param('variation', String(d));
                announce(patchLabel() + ', bank ' + value(fields[2]) + ', ' + value(fields[3]));
            }
            redraw = true;
        }
        return;
    }
    if (cc === 50 && v) {
        selected = selected === 0 ? 1 : 0;
        editing = false;
        speak();
        redraw = true;
    } else if (cc === 3 && v) {
        if (selected === 0) {
            browser = 'groups';
            const bank = value(fields[2]);
            group = bank === 127 ? 1 : bank === 126 ? 2 : 0;
            announce('Browse, ' + groups[group][0]);
        } else { editing = !editing; speak(); }
        redraw = true;
    } else if (cc === 14) {
        const d = decodeDelta(v);
        if (!d) return;
        if (selected === 0) {
            host_module_set_param('browse', String(d));
            announce(patchLabel());
        } else if (editing) {
            const field = fields[selected];
            const next = Math.max(field[3] || 0, Math.min(field[2], value(field) + d));
            host_module_set_param(field[0], String(next));
            if (field[0] === 'part') announce(field[1] + ', ' + String.fromCharCode(65 + Math.floor(next / 16)) + (next % 16 + 1));
            else if (field[0] === 'octave' && next !== 0) announce(field[1] + ', ' + (next < 0 ? 'minus ' : 'plus ') + Math.abs(next));
            else announce(field[1] + ', ' + next);
        } else { selected = 1 + ((selected - 1 + d) % (fields.length - 1) + fields.length - 1) % (fields.length - 1); speak(); }
        redraw = true;
    }
}
function onMidiMessageExternal() {}

Object.assign(globalThis, { init, tick, onMidiMessageInternal, onMidiMessageExternal });
globalThis.chain_ui = { init, tick, onMidiMessageInternal, onMidiMessageExternal };
