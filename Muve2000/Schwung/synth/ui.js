import { decodeDelta, decodeAcceleratedDelta, isCapacitiveTouchMessage } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

const knobs = [['volume', 'Volume'], ['pan', 'Pan'], ['cutoff', 'Cutoff'], ['resonance', 'Resonance'],
    ['attack', 'Attack'], ['release', 'Release'], ['reverb', 'Reverb Send'], ['chorus', 'Chorus Send']];
const get = key => String(host_module_get_param(key) ?? '');
const set = (key, value) => host_module_set_param(key, String(value));
const num = key => Number(get(key)) || 0;
const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
const partName = part => String.fromCharCode(65 + Math.floor(part / 16)) + (part % 16 + 1);
let pages, editing, shift, dirty, status, ticks, lastKnob, lastTurn;
const page = () => pages[pages.length - 1];
const item = () => page().rows[page().index];
// The highlighted track can differ from the slot whose editor is open.
let editorSlot = -1, lastChannel;
function syncTrack() {
    if (typeof shadow_get_slots !== 'function' || typeof shadow_get_param !== 'function') return;
    const slots = shadow_get_slots();
    if (!slots) return;
    if (editorSlot < 0) {
        const id = get('editor_instance');
        if (!id) return;
        editorSlot = slots.findIndex((_, i) => shadow_get_param(i, 'synth:editor_instance') === id);
    }
    if (editorSlot < 0 || !slots[editorSlot]) return;
    const forward = shadow_get_param(editorSlot, 'slot:forward_channel');
    if (forward == null || forward === '') return;
    const channel = Number(forward) >= 0 ? Number(forward) : Number(slots[editorSlot].channel) - 1;
    if (Number.isInteger(channel) && channel >= -1 && channel < 16 && channel !== lastChannel) {
        if (set('track_channel', channel) !== false) lastChannel = channel;
    }
}

function say() {
    const r = item(), value = r.value?.();
    announce(r.name + (value ? ', ' + value : ''));
}
function open(title, rows, index = 0) {
    const child = pages.length > 0;
    if (child) rows.unshift({name:'Back', run:back});
    pages.push({title, rows, index:child && rows.length > 1 ? index + 1 : 0});
    editing = false; dirty = true;
    say();
}
function back() {
    if (editing) editing = false;
    else if (pages.length > 1) pages.pop();
    dirty = true; say();
}
function catalogue(title, group, variations = false) {
    if (!variations) set('group', group);
    const bank = num('bank'), lsb = num('lsb'), pc = num('program');
    const current = variations ? (bank << 14 | lsb << 7 | pc) : pc;
    const key = variations ? 'variation_rows:' : 'patch_rows:';
    const rows = [];
    for (let start = 0; ; start += 8) {
        const lines = get(key + start).trimEnd().split('\n');
        const more = lines[lines.length - 1] === '+';
        if (more) lines.pop();
        for (const line of lines) {
            const split = line.indexOf('\t');
            if (split < 1) continue;
            const id = Number(line.slice(0, split));
            rows.push({id, name:line.slice(split + 1), preview:() => set(variations ? 'voice_address' : 'program', id), run:back});
        }
        if (!more) break;
    }
    const index = Math.max(0, rows.findIndex(r => r.id === current));
    if (rows[index] && rows[index].id !== current) rows[index].preview();
    open(title, rows, index);
}
function field(name, key, lo, hi, format = String) {
    return {name, value:() => format(num(key)), step:delta => set(key, clamp(num(key) + delta, lo, hi))};
}
function settings() {
    open('Settings', [
        field('Edit part','edit_target',-1,63,p => p < 0 ? 'Follow track, ' + partName(num('part')) : partName(p)),
        field('Bank MSB','bank',0,127), field('Bank LSB','lsb',0,127),
        field('Octave','octave',-4,4,n => n < 0 ? 'minus ' + (-n) : n > 0 ? 'plus ' + n : '0'),
        field('Pad response','fast_pads',0,1,n => n ? 'Fast' : 'Buffered')
    ]);
}
function init() {
    pages=[]; editing=shift=false; dirty=true; status=''; ticks=0; lastKnob=-1; lastTurn=0;
    editorSlot=-1; lastChannel=undefined; syncTrack();
    open('Muve2000', [
        {name:'Instruments',run:() => catalogue('Instruments',0)},
        {name:'Drums',run:() => catalogue('Drums',127)},
        {name:'SFX Kits',run:() => catalogue('SFX Kits',126)},
        {name:'Bank Variations',run:() => catalogue('Bank Variations',0,true)},
        {name:'Settings',run:settings}
    ]);
}
function tick() {
    if (!pages) init();
    if (!(ticks++ % 16)) {
        syncTrack();
        const next = get('status');
        if (next !== status) {
            status=next; dirty=true;
            if (next === 'Ready.') say(); else if (next) announce(next);
        }
    }
    if (!dirty) return;
    dirty=false; clear_screen();
    drawMenuHeader(page().title, editing ? 'EDIT' : partName(num('part')));
    if (status !== 'Ready.') { print(2,20,status.slice(0,21),1); print(2,31,status.slice(21,42),1); }
    else {
        drawMenuList({items:page().rows, selectedIndex:page().index,
            listArea:{topY:menuLayoutDefaults.listTopY,bottomY:menuLayoutDefaults.listBottomWithFooter},
            getLabel:r=>r.name, getValue:r=>r.value?.() || '', valueAlignRight:true, announce:false});
        drawMenuFooter(editing ? 'Jog edit, Click done' : item().preview ? 'Jog sound, Click done' : 'Jog move, Click open');
    }
    if (typeof host_flush_display === 'function') host_flush_display();
}
function onMidiMessageInternal(data) {
    if (!pages || !data || data.length < 3) return;
    const type=data[0]&240, cc=data[1], v=data[2];
    if (type===176 && cc===49) {shift=!!v;return;}
    if (status !== 'Ready.') return;
    const touch=type===144 && v && cc<8, turn=type===176 && cc>=71 && cc<79;
    if (touch || turn) {
        const index=touch ? cc : cc-71, [key,name]=knobs[index];
        if (turn) set('step:'+key,decodeAcceleratedDelta(v,cc));
        const now=Date.now();
        announce((touch || index!==lastKnob || now-lastTurn>1500 ? name+', ' : '')+num(key));
        lastKnob=index;lastTurn=now;dirty=true;return;
    }
    if(type!==176 || isCapacitiveTouchMessage(data)) return;
    if(cc===50 && v || cc===3 && v && shift) {back();return;}
    if(cc===3 && v) {
        if(item().step) {editing=!editing;say();}
        else item().run();
    } else if(cc===14) {
        const delta=decodeDelta(v);
        if(!delta) return;
        if(editing) item().step(delta);
        else {
            const next=clamp(page().index+delta,0,page().rows.length-1);
            if(next===page().index) return;
            page().index=next;
            item().preview?.();
        }
        say();
    } else return;
    dirty=true;
}
function onMidiMessageExternal() {}
Object.assign(globalThis,{init,tick,onMidiMessageInternal,onMidiMessageExternal});
globalThis.chain_ui={init,tick,onMidiMessageInternal,onMidiMessageExternal};
