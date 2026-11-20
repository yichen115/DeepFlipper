/* Execute the delivered renderer and validate real RGBA output after input. */
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const path = process.argv[2];
assert(path, 'Pass the generated room preview HTML path');
const html = fs.readFileSync(path, 'utf8');
const payload = html.match(/<script id="whale-room-data" type="application\/json">([\s\S]*?)<\/script>/)[1];
const source = html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const assets = JSON.parse(payload);
const raw = Buffer.from(assets.pixels, 'base64');
const mask = Buffer.from(assets.masks, 'base64');
assert.strictEqual(raw.length, mask.length);
for (let frame=0;frame<raw.length/512;frame++) {
  assert(mask.subarray(frame*512,(frame+1)*512).some(value=>value!==0), `Invisible pet frame ${frame}`);
}
for(let i=0;i<raw.length;i++) assert.strictEqual(raw[i] & ~mask[i],0,`Uncovered sprite ink byte ${i}`);
class Element {
  constructor(dataset={}) {this.dataset=dataset;this.listeners={};this.attributes={};this.textContent='';this.image=null;}
  addEventListener(name,fn){this.listeners[name]=fn;}
  setAttribute(name,value){this.attributes[name]=value;}
  click(){assert(this.listeners.click);this.listeners.click();}
  getContext(){
    return {
      createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),
      putImageData:image=>{this.image=Uint8Array.from(image.data);}
    };
  }
}
const actions=['idle','pat','rice','listening','speaking','sleeping'].map(careAction=>new Element({careAction}));
const pages=['home','care','stats','system'].map(roomPage=>new Element({roomPage}));
const ids=Object.fromEntries(['whale-room-data','whale-room-screen','whale-room-native','whale-room-caption','whale-room-replay'].map(id=>[id,new Element()]));
ids['whale-room-data'].textContent=payload;
const root={
  querySelector:q=>{assert(ids[q.slice(1)],q);return ids[q.slice(1)];},
  querySelectorAll:q=>{const list={'[data-care-action]':actions,'[data-room-page]':pages}[q];assert(list,q);return list;}
};
let timer=null;
vm.runInNewContext(source,{
  document:{getElementById:id=>{assert.strictEqual(id,'whale-home-studio');return root;}},
  window:{matchMedia:()=>({matches:false})},atob:s=>Buffer.from(s,'base64').toString('binary'),
  setInterval:fn=>{timer=fn;return 1;},clearInterval:()=>{timer=null;}
},{timeout:2000});
const preview=root.__roomPreview;
const screen=ids['whale-room-screen'];
const snapshots=[];
function palette(){
  const colors=new Set();
  for(let i=0;i<8192;i++) {
    const r=screen.image[i*4],g=screen.image[i*4+1],b=screen.image[i*4+2];
    assert.strictEqual(screen.image[i*4+3],255,'Every frame must be opaque');
    colors.add([r,g,b].join(','));
  }
  assert.deepStrictEqual([...colors].sort(),['239,182,72','41,31,23']);
}
for(const button of actions){
  button.click();preview.stop();
  assert.strictEqual(preview.state.action,button.dataset.careAction);
  assert.strictEqual(preview.state.page,'home');
  const clip=assets.animations.find(c=>c.id===preview.state.action);
  for(let i=0;i<clip.frames.length;i++){
    preview.state.frame=i;preview.draw();
    const pixels=preview.renderPixels(),frame=clip.frames[i];
    let faceInk=0;
    // Check face/body pixels, not just the fact that a room was rendered.
    for(let y=19;y<39;y++)for(let x=12;x<42;x++){
      const address=frame*512+y*8+(x>>3),bit=x&7;
      if((mask[address]>>bit)&1){
        const wanted=(raw[address]>>bit)&1;
        assert.strictEqual(pixels[y*128+x+33],wanted,`Missing pet: ${preview.state.action}/${i}/${x}/${y}`);
        faceInk+=wanted;
      }
    }
    assert(faceInk>100,'Pet must be visibly present in the center');
    palette();
  }
  preview.state.frame=Math.floor(clip.frames.length*.6);preview.draw();
  const baseline=Buffer.from(screen.image);
  for(let n=0;n<8;n++)for(const page of pages){page.click();palette();}
  pages[0].click();
  assert.deepStrictEqual(Buffer.from(screen.image),baseline,'Returning home must not leave overlay ghosts');
  assert.deepStrictEqual(Buffer.from(ids['whale-room-native'].image),baseline,'Large/native previews must agree');
  snapshots.push({action:preview.state.action,pixels:Array.from(preview.renderPixels())});
}
const idle=actions[0];idle.click();
let ticks=0;
while(timer && ticks<110){timer();ticks++;}
assert(!timer&&ticks>0,'Replay must terminate');
palette();
if(process.argv[3])fs.writeFileSync(process.argv[3],JSON.stringify(snapshots));
console.log('PASS: 340 animated frames retain the pet; all overlays return to identical opaque pixels without trails.');
