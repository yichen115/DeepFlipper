const assert=require('node:assert/strict');
const preview=require('../firmware/esp32/main/boards/deepflipper-s3/web/audio_preview.js');
class Element{constructor(){this.value='';this.hidden=true;this.disabled=false;this.textContent='';this.files=[];}pause(){}removeAttribute(){} }
(async()=>{
  const pcm=preview.pcm16(new Float32Array([-1,-0.5,0,0.5,1]));const data=new DataView(pcm);
  assert.equal(data.getInt16(0,true),-32768);assert.equal(data.getInt16(8,true),32767);
  const wav=new DataView(preview.wav(pcm));assert.equal(wav.getUint32(24,true),24000);assert.equal(wav.getUint16(22,true),1);assert.equal(wav.getUint32(40,true),10);
  assert.throws(()=>preview.pcm16(new Float32Array([NaN])));
  assert.throws(()=>preview.pcm16(new Float32Array(24000*30+1)));
  const tone=new DataView(preview.tone());assert.equal(tone.byteLength,96000);assert.equal(tone.getInt16(0,true),0);assert.equal(tone.getInt16(tone.byteLength-2,true),0);
  const calls=[],signal=new AbortController();
  const api=async(path,body,options)=>{calls.push({path,body,options});return {id:'test'};};
  await preview.upload(api,new ArrayBuffer(100000),signal.signal,()=>{},()=>{});
  assert.deepEqual(calls.map(c=>c.path),['/api/audio/start','/api/audio/chunk','/api/audio/chunk','/api/audio/commit']);
  assert.equal(calls[0].body.samples,50000);assert.equal(calls[1].body.byteLength,65536);assert.equal(calls[2].body.byteLength,34464);
  assert.equal(calls[2].options.headers['X-PCM-Offset'],'65536');
  const abort=new AbortController();let committed=false;
  await assert.rejects(preview.upload(async(path)=>{if(path.endsWith('chunk'))abort.abort();if(path.endsWith('commit'))committed=true;return {id:'x'};},new ArrayBuffer(100000),abort.signal,()=>{},()=>{}),{name:'AbortError'});
  assert.equal(committed,false);
  const elements=new Map();const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
  let resolveStart;const paths=[];
  const controller=new preview.Controller({get,api:async(path)=>{paths.push(path);if(path.endsWith('start'))return new Promise(resolve=>{resolveStart=resolve;});return {};},message:()=>{},onStatus:async()=>{}});
  controller.selectTone();controller.refresh({flipper_connected:true,volume:50,mute:false,app_state:'idle'});
  assert.equal(get('preview-play').disabled,false);
  const running=controller.play();controller.reset();resolveStart({id:'old'});await running;
  assert.deepEqual(paths,['/api/audio/start']);assert.equal(controller.pcm,null);
  console.log('PASS: preview WAV/tone, ordered raw upload, cancellation and stale logout isolation');
})().catch(error=>{console.error(error);process.exitCode=1;});
