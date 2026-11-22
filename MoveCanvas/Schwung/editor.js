import * as os from 'os';
import {decodeDelta, decodeAcceleratedDelta, isCapacitiveTouchMessage} from '/data/UserData/schwung/shared/input_filter.mjs';
import {drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {announce} from '/data/UserData/schwung/shared/screen_reader.mjs';
import {buildFilepathBrowserState,refreshFilepathBrowser,moveFilepathBrowserSelection,activateFilepathBrowserItem} from '/data/UserData/schwung/shared/filepath_browser.mjs';

const model = /* MODULE_DATA */;
const maps = ['', 'SC-55', 'SC-88', 'SC-88 Pro', 'SC-8820'];
const families = ['Piano','Chromatic percussion','Organ','Guitar','Bass','Strings','Ensemble','Brass','Reed','Pipe','Synth lead','Synth pad','Synth effects','Ethnic','Percussive','Sound effects'];
const units = {
    Switch:['Off','On'], LowFrequency:['200 Hz','400 Hz'], HighFrequency:['3 kHz','6 kHz'],
    ReverbType:['Room 1','Room 2','Room 3','Hall 1','Hall 2','Plate','Delay','Panning Delay'],
    ChorusType:['Chorus 1','Chorus 2','Chorus 3','Chorus 4','Feedback chorus','Flanger','Short delay','Short delay feedback'],
    DelayType:['Delay 1','Delay 2','Delay 3','Delay 4','Pan delay 1','Pan delay 2','Pan delay 3','Pan delay 4','Delay to reverb','Pan repeat'],
    InsertionType:model.types
};
const root = '/data/UserData/UserLibrary';
const fs = {stat:path=>os.stat(path),readdir(path){const a=os.readdir(path)||[];return Array.isArray(a[0])?a[0]:a;}};
const get = key=>String(host_module_get_param(key)??'');
const set = (key,value)=>host_module_set_param(key,String(value));
const num = key=>Number(get(key))||0;
const limit = (x,lo,hi)=>Math.max(lo,Math.min(hi,x));
const signed = x=>x<0?'minus '+(-x):x>0?'plus '+x:'0';
const time = ms=>Math.floor(ms/60000)+' min '+Math.floor(ms/1000)%60+' sec';
let stack, editing, browser, folder=root, changed, shift, ticks, previous, wakeSpeech;
let releasePlay=false;
function format(index,value) {
    const [key,,,,,unit]=model.controls[index];
    if(key==='part'||key==='program'||unit==='Setup') return String(value+1);
    if(key==='map') return maps[value];
    if(key==='routing') return value?'Selected part':'Multitimbral';
    if(unit==='DrumPitch'||unit==='Signed64') return signed(value-(unit==='DrumPitch'?60:64));
    if(unit==='Pan'||key==='pan') return value===0&&unit==='Pan'?'Random':value===64?'Centre':value<64?'Left '+(64-value):'Right '+(value-64);
    return units[unit]?.[value]??String(value);
}
const page=()=>stack[stack.length-1];
const item=()=>page().rows[page().index];
const label=i=>i?.label||i?.name||i?.filename||i?.kind||'Empty folder';
function speak(){if(browser)announce(label(browser.items[browser.selectedIndex]));else if(item())announce([item().name,item().value?.()].filter(Boolean).join(', '));}
function menu(title,rows,knobs) {
    stack.push({title,rows,index:0,knobs});editing=false;changed=true;speak();
}
function back(){
    if(browser)browser=null;
    else if(editing)editing=false;
    else if(stack.length>1)stack.pop();
    changed=true;speak();
}
function field(index) {
    const [key,name,low,high]=model.controls[index];
    return {name,value:()=>format(index,num(key)),step(d){
        const n=limit(num(key)+d,low,high);set(key,n);announce(name+', '+format(index,n));
    }};
}
function pickGroup(map,drums,family){
    const rows=[];
    model.programs.forEach(([name,m,bank,pc,kit],index)=>{
        if(m!==map || kit!==drums || (!drums && Math.floor(pc/8)!==family))return;
        rows.push({name,value:()=>'Bank '+bank+', program '+(pc+1),run(){
            set('preset',index);announce(name+', '+maps[map]+', part '+(kit?10:num('part')===9?1:num('part')+1));
        }});
    });
    menu(drums?'Drum kits':families[family],rows);
}
function banks() {
    menu('Sound maps',maps.slice(1).map((name,j)=>({name,run:()=>menu(name,[
        {name:'Programs',run:()=>menu('Program family',families.map((name,k)=>({name,run:()=>pickGroup(j+1,false,k)})))},
        {name:'Drum kits',run:()=>pickGroup(j+1,true,-1)}])})));
}
function openFile() {
    browser=buildFilepathBrowserState({root,filter:['.mid','.midi','.kar','.rmi'],name:'MIDI files',start_path:folder},'');
    refreshFilepathBrowser(browser,fs);changed=true;speak();
}
function toggle(key,name){const v=1-num(key);set(key,v);announce(name+', '+(v?'On':'Off'));}
function play(){
    if(!get('loaded')){openFile();return;}
    if(num('follow')&&typeof move_midi_inject_to_move==='function'){
        move_midi_inject_to_move([11,176,85,127]);releasePlay=true;return;
    }
    const on=!!num('playing');set(on?'stop':'play',1);announce(on?'Paused':'Play');
}
function status(){announce([get('status'),num('preparing')?num('render_percent')+' percent':get('preset_name'),time(num('position_ms')),'Audio gaps '+num('underruns'),'Dropped MIDI messages '+num('midi_drops')].join('. '));}
function init() {
    stack=[];browser=null;editing=shift=false;previous='';ticks=0;wakeSpeech=true;
    const rows=model.tool?[
        {name:'Open MIDI file',run:openFile},{name:'Play or stop',run:play},
        {name:'Playback mode',value:()=>num('render_first')?'Render First':'Live',run:()=>{set('render_first',1-num('render_first'));announce('Playback mode changed');}},
        {name:'Follow Move transport',value:()=>num('follow')?'On':'Off',run:()=>toggle('follow','Follow Move')},
        {name:'Lock selected part patch',value:()=>num('part_lock')?'On':'Off',run:()=>toggle('part_lock','Patch lock')},
        {name:'Back to start',run:()=>{set('seek',0);announce('Start');}},
        {name:'Loop',value:()=>num('loop')?'On':'Off',run:()=>toggle('loop','Loop')},
        {name:'USB MIDI input',value:()=>num('usb')?'On':'Off',run:()=>toggle('usb','USB MIDI input')}
    ]:[];
    rows.push({name:'Preset bank',value:()=>get('preset_name'),run:banks});
    for(const [name,indices] of model.pages)rows.push({name,run:()=>menu(name,indices.map(field),indices)});
    rows.push({name:'All notes off',run:()=>{set('panic',1);announce('All notes off');}},
              {name:'Status',run:status});
    menu(model.tool?'Move Canvas MIDI':'Move Canvas',rows);
}
function tick() {
    if(releasePlay){releasePlay=false;move_midi_inject_to_move([11,176,85,0]);}
    if(!stack)init();
    if(++ticks%12===0){const s=get('status');if(s!==previous){previous=s;if(s)announce(s);changed=true;}}
    if(!changed)return;
    changed=false;clear_screen();
    drawMenuHeader(browser?'MIDI files':page().title,editing?'EDIT':'');
    drawMenuList({items:browser?browser.items:page().rows,selectedIndex:browser?browser.selectedIndex:page().index,
        listArea:{topY:menuLayoutDefaults.listTopY,bottomY:menuLayoutDefaults.listBottomWithFooter},
        getLabel:i=>browser?label(i):i.name,getValue:i=>browser?'':i.value?.()||'',valueAlignRight:true,announce:false});
    drawMenuFooter(browser?'Click open, Menu back':editing?'Jog edit, Click done':'Click select, Menu back');
    if(typeof host_flush_display==='function')host_flush_display();
    if(wakeSpeech){wakeSpeech=false;speak();}
}
function onMidiMessageInternal(data) {
    if(!stack||!data||data.length<3)return;
    const [s,key,v]=data,type=s&240;
    if(type===176&&key===49){shift=!!v;return;}
    if(model.tool&&type===176&&key===51&&v&&!shift&&typeof host_suspend_overtake==='function'){host_suspend_overtake();return;}
    if(model.tool&&type===176&&key===85&&v){if(shift){set('seek',0);announce('Start');}else play();return;}
    if(model.tool&&type===176&&key===58&&v){toggle('loop','Loop');return;}
    if(model.tool&&!browser&&type===144&&v&&key>=68&&key<84){
        if(num('render_first')&&num('playing')){announce('Switch to Live for mixer edits during playback');return;}
        const ch=key-68;toggle((shift?'solo:':'mute:')+ch,'Part '+(ch+1)+(shift?' solo':' mute'));return;
    }
    const touch=type===144&&v&&key<8,turn=type===176&&key>=71&&key<79;
    if(!browser&&(touch||turn)){
        const ix=turn?key-71:key,d=turn?decodeAcceleratedDelta(v,key):0;
        if(model.tool&&stack.length===1){
            if(turn&&num('render_first')&&num('playing')){announce('Switch to Live for mixer edits during playback');return;}
            const ch=ix+(shift?8:0),id='trim:'+ch,n=limit(num(id)+d,0,100);if(turn)set(id,n);
            announce('Part '+(ch+1)+', level '+n+' percent');return;
        }
        const control=(page().knobs||model.knobs)[ix+(page().knobs&&shift?8:0)];
        if(control===undefined)return;
        const f=field(control);if(turn)f.step(d);else announce(f.name+', '+f.value());
        changed=true;return;
    }
    if(type!==176||isCapacitiveTouchMessage(data))return;
    if(key===50&&v){back();return;}
    if(key===14){
        const d=decodeDelta(v);if(!d)return;
        if(browser){moveFilepathBrowserSelection(browser,d);speak();}
        else if(model.tool&&shift&&stack.length===1){const ms=limit(num('position_ms')+d*5000,0,num('duration_ms'));set('seek',ms);announce(time(ms));}
        else if(editing)item().step(d);
        else {page().index=limit(page().index+d,0,page().rows.length-1);speak();}
    }else if(key===3&&v){
        if(browser){
            const result=activateFilepathBrowserItem(browser);
            if(result.action==='open'){refreshFilepathBrowser(browser,fs);speak();}
            else if(result.action==='select'){folder=result.value.slice(0,result.value.lastIndexOf('/'));set('file_path',result.value);browser=null;announce('Opening '+result.filename);}
        }else if(shift)status();
        else if(item()?.step){editing=!editing;announce((editing?'Edit ':'')+item().name+', '+item().value());}
        else item()?.run?.();
    }else return;
    changed=true;
}
function onResume(){changed=wakeSpeech=true;}
function onMidiMessageExternal(){}
Object.assign(globalThis,{init,tick,onResume,onMidiMessageInternal,onMidiMessageExternal});
globalThis.chain_ui={init,tick,onMidiMessageInternal,onMidiMessageExternal};
