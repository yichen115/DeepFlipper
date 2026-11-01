(function(root){
'use strict';
const RATE=24000,MAX_SECONDS=30,CHUNK=65536;
function pcm16(samples){
  if(!samples.length||samples.length>RATE*MAX_SECONDS)throw new Error('音频需要在 30 秒以内。');
  const out=new ArrayBuffer(samples.length*2),view=new DataView(out);
  for(let i=0;i<samples.length;i++){
    if(!Number.isFinite(samples[i]))throw new Error('音频包含无效采样。');
    const value=Math.max(-1,Math.min(1,samples[i]));
    view.setInt16(i*2,Math.round(value*(value<0?32768:32767)),true);
  }
  return out;
}
function wav(pcm){
  const out=new ArrayBuffer(44+pcm.byteLength),v=new DataView(out);
  const text=(offset,s)=>{for(let i=0;i<s.length;i++)v.setUint8(offset+i,s.charCodeAt(i));};
  text(0,'RIFF');v.setUint32(4,36+pcm.byteLength,true);text(8,'WAVE');text(12,'fmt ');
  v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,RATE,true);
  v.setUint32(28,RATE*2,true);v.setUint16(32,2,true);v.setUint16(34,16,true);text(36,'data');
  v.setUint32(40,pcm.byteLength,true);new Uint8Array(out,44).set(new Uint8Array(pcm));return out;
}
function tone(){
  const samples=new Float32Array(RATE*2),fade=RATE*0.02;
  for(let i=0;i<samples.length;i++)samples[i]=Math.sin(2*Math.PI*440*i/RATE)*0.15*Math.min(1,i/fade,(samples.length-1-i)/fade);
  return pcm16(samples);
}
function metadata(blob){
  return new Promise((resolve,reject)=>{
    const player=document.createElement('audio'),url=URL.createObjectURL(blob);
    let finished=false;
    const done=(error)=>{if(finished)return;finished=true;clearTimeout(timer);player.removeAttribute('src');player.load();URL.revokeObjectURL(url);error?reject(error):resolve();};
    const timer=setTimeout(()=>done(new Error('读取音频时长超时，请换一个文件。')),10000);
    player.onloadedmetadata=()=>done(!Number.isFinite(player.duration)||player.duration<=0||player.duration>MAX_SECONDS?new Error('请选择 30 秒以内的音频。'):null);
    player.onerror=()=>done(new Error('浏览器无法解码这个音频，请使用 WAV、MP3 或浏览器支持的格式。'));
    player.preload='metadata';player.src=url;
  });
}
async function decode(blob){
  if(blob.size>10*1024*1024)throw new Error('文件超过 10 MB，请截取 30 秒以内的片段。');
  await metadata(blob);
  const Context=root.AudioContext||root.webkitAudioContext,Offline=root.OfflineAudioContext||root.webkitOfflineAudioContext;
  if(!Context||!Offline)throw new Error('当前浏览器缺少音频解码支持，可先使用连续测试音。');
  const context=new Context();let decoded;
  try{decoded=await context.decodeAudioData(await blob.arrayBuffer());}
  finally{await context.close();}
  if(!Number.isFinite(decoded.duration)||decoded.duration<=0||decoded.duration>MAX_SECONDS||decoded.numberOfChannels>8)throw new Error('音频时长或声道数超出限制。');
  const length=Math.ceil(decoded.duration*RATE),offline=new Offline(1,length,RATE),source=offline.createBufferSource();
  source.buffer=decoded;source.connect(offline.destination);source.start();
  const rendered=await offline.startRendering();return pcm16(rendered.getChannelData(0));
}
async function upload(api,pcm,signal,onProgress,onId){
  const data=new Uint8Array(pcm);
  const start=await api('/api/audio/start',{sample_rate:RATE,samples:data.length/2},{signal});
  onId(start.id);
  for(let offset=0;offset<data.length;offset+=CHUNK){
    if(signal.aborted)throw new DOMException('Aborted','AbortError');
    const part=data.subarray(offset,Math.min(data.length,offset+CHUNK));
    await api('/api/audio/chunk',part,{signal,raw:true,headers:{'X-Preview-ID':start.id,'X-PCM-Offset':String(offset)}});
    onProgress(offset+part.length,data.length);
  }
  if(signal.aborted)throw new DOMException('Aborted','AbortError');
  await api('/api/audio/commit',{id:start.id},{signal});
  return start.id;
}
class Controller{
  constructor({get,api,message,onStatus}){
    this.get=get;this.api=api;this.message=message;this.onStatus=onStatus;
    this.pcm=null;this.url=null;this.generation=0;this.preparing=false;this.uploading=false;this.pending=false;this.id='';this.live=null;this.writing=false;
    get('preview-sample').onclick=()=>this.prepareSample();
    get('preview-tone').onclick=()=>this.selectTone();
    get('preview-file').onchange=()=>{const file=get('preview-file').files[0];if(file)this.prepare(file,file.name);};
    get('preview-play').onclick=()=>this.play();get('preview-stop').onclick=()=>this.stop();
  }
  reset(){
    this.generation++;if(this.abort)this.abort.abort();this.abort=null;
    this.pcm=null;this.preparing=this.uploading=this.pending=false;this.id='';
    const player=this.get('preview-local');player.pause();player.removeAttribute('src');player.hidden=true;
    if(this.url)URL.revokeObjectURL(this.url);this.url=null;
    this.get('preview-file').value='';this.get('preview-selected').textContent='请选择一种测试音频。';this.refresh();
  }
  setPcm(pcm,label){
    this.pcm=pcm;
    if(this.url)URL.revokeObjectURL(this.url);
    this.url=URL.createObjectURL(new Blob([wav(pcm)],{type:'audio/wav'}));
    const player=this.get('preview-local');player.src=this.url;player.hidden=false;
    this.get('preview-selected').textContent=`${label} · ${(pcm.byteLength/(RATE*2)).toFixed(2)} 秒 · 24 kHz 单声道`;
  }
  async prepare(blob,label){
    const generation=++this.generation;this.preparing=true;this.pcm=null;this.refresh();
    this.get('preview-local').pause();this.get('preview-local').hidden=true;
    this.get('preview-selected').textContent='正在解码并准备对比音频…';
    try{const pcm=await decode(blob);if(generation===this.generation)this.setPcm(pcm,label);}
    catch(error){if(generation===this.generation){this.message(error.message,true);this.get('preview-selected').textContent='准备失败，请重新选择音频。';}}
    finally{if(generation===this.generation){this.preparing=false;this.refresh();}}
  }
  async prepareSample(){
    const generation=++this.generation;this.preparing=true;this.refresh();
    try{const response=await fetch('/audio-preview.wav',{credentials:'same-origin'});if(!response.ok)throw new Error('读取内置人声失败，请更新 ESP32 固件。');const blob=await response.blob();if(generation===this.generation)await this.prepare(blob,'内置人声');}
    catch(error){if(generation===this.generation){this.preparing=false;this.message(error.message,true);this.refresh();}}
  }
  selectTone(){this.generation++;this.preparing=false;this.setPcm(tone(),'440 Hz 连续测试音');this.refresh();}
  refresh(live=this.live,writing=this.writing){
    this.live=live;this.writing=writing;
    const state=live?.audio_preview||{},active=Boolean(state.busy)||this.pending;
    for(const id of ['preview-file','preview-sample','preview-tone'])this.get(id).disabled=this.preparing||this.uploading||active||writing;
    const ready=live?.flipper_connected&&!live.mute&&live.volume>0&&!live.probing&&!live.applying&&!live.wifi_pending&&['idle','starting'].includes(live.app_state);
    this.get('preview-play').disabled=!this.pcm||this.preparing||this.uploading||active||writing||!ready;
    this.get('preview-stop').disabled=!this.uploading&&!active;
    if(!this.uploading){
      const labels={idle:'尚未播放',uploading:'正在接收音频',queued:'正在准备扬声器',playing:'ESP32 正在播放',stopping:'正在停止',completed:'设备播放流程已结束',cancelled:'已停止播放',failed:'播放失败'};
      const errors={upload_timeout:'上传超时，请重新上传',audio_prepare_timeout:'设备未及时准备好音频，请重试',playback_interrupted:'设备状态变化或输出不可用，播放已中断'};
      this.get('preview-state').textContent=(labels[state.state]||'请选择音频，然后点击“在 ESP32 播放”')+(state.error?' · '+(errors[state.error]||state.error):'');
    }
    if(this.id&&state.id===this.id&&['completed','cancelled','failed'].includes(state.state)&&!state.busy){this.pending=false;this.id='';this.get('preview-play').disabled=!this.pcm||writing||!ready;this.get('preview-stop').disabled=true;}
    this.get('preview-volume').textContent=live?`设备音量 ${live.volume}%${live.mute?'（已静音）':''}；电脑播放器音量单独控制。`:'等待设备状态。';
  }
  async play(){
    if(!this.pcm||this.uploading||this.preparing||this.pending||this.writing)return;
    const generation=this.generation;
    let id='';
    this.uploading=true;this.pending=false;this.id='';this.abort=new AbortController();this.refresh();
    try{
      id=await upload(this.api,this.pcm,this.abort.signal,(received,total)=>{if(generation===this.generation)this.get('preview-state').textContent=`正在上传 ${Math.round(received*100/total)}%（上传完整后播放）`;},value=>{id=value;if(generation===this.generation)this.id=value;});
      if(generation!==this.generation)return;
      this.id=id;
      this.pending=true;this.message('音频已上传，ESP32 即将播放。请与上方电脑播放器对比。');await this.onStatus();
    }catch(error){
      if(generation!==this.generation)return;
      try{await this.api('/api/audio/stop',id?{id}:{});}catch{}
      this.pending=false;if(error.name!=='AbortError'&&!error.stale)this.message(error.message||'上传失败，请重新尝试。',true);
    }finally{if(generation===this.generation){this.uploading=false;this.abort=null;this.refresh();}}
  }
  async stop(){
    const generation=this.generation,id=this.id;
    if(this.abort)this.abort.abort();
    try{await this.api('/api/audio/stop',id?{id}:{});if(generation!==this.generation)return;this.pending=false;this.message('已请求停止 ESP32 播放。');await this.onStatus();}
    catch(error){if(generation===this.generation&&!error.stale)this.message(error.message||'暂时无法停止，请恢复设备连接。',true);}
    if(generation===this.generation)this.refresh();
  }
}
const exported={RATE,MAX_SECONDS,CHUNK,pcm16,wav,tone,upload,Controller};
if(typeof module!=='undefined'&&module.exports)module.exports=exported;else root.DfAudioPreview=exported;
})(globalThis);
