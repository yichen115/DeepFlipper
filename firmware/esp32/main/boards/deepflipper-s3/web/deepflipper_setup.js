(function () {
'use strict';
class DraftBook {
  constructor() { this.groups = Object.fromEntries(['wifi','server','audio'].map(key => [key,{revision:0, edits:0, fields:new Set()}])); }
  touch(group, field) { const g=this.groups[group]; g.fields.add(field); g.edits++; }
  ticket(group) { const g=this.groups[group]; return {group,revision:g.revision,edits:g.edits,fields:[...g.fields]}; }
  merge(data, options={}) {
    const apply=[];
    for(const [name,g] of Object.entries(this.groups)) {
      const acknowledged=options.ack?.group===name && options.ack.edits===g.edits;
      if(!g.fields.size || acknowledged) { g.revision=data.revision;g.fields.clear();apply.push(name); }
      else if(options.rebase || (options.own && g.revision===options.own.before && data.revision===options.own.after && options.own.after===options.own.before+1)) g.revision=data.revision;
    }
    return apply;
  }
}
function wifiOutcome(trial, expected) {
  if(!expected || !trial || trial.request_id!==expected.request_id) return null;
  return trial.state==='saved'?'saved':trial.state==='failed'?'failed':null;
}
function wifiReason(reason) {
  if((reason||'').includes('association_failed')) return 'Wi-Fi 连接或认证未完成，请检查信号、密码和加密方式。';
  if((reason||'').includes('dhcp_timeout')) return '网络已关联，但未能获取 IP，请检查路由器 DHCP。';
  if(reason==='disconnect_timeout') return '旧网络尚未断开，请稍后重试。';
  return reason==='connection_timeout'?'连接超时，请确认是可用的 2.4 GHz 网络。':reason||'';
}
function actionState(data) {
  let talk='',speaker='';
  const busy=data.probing || data.wifi_pending || data.applying || data.audio_preview?.busy;
  if(!data.flipper_connected) speaker='module_offline';
  else if(data.mute) speaker='speaker_muted';
  else if(data.volume===0) speaker='volume_zero';
  else if(busy || !['idle','starting'].includes(data.app_state)) speaker='busy';
  if(data.wifi!=='connected') talk='wifi_offline';
  else if(!data.flipper_connected) talk='module_offline';
  else if(!data.config_ready) talk='needs_config';
  else if(!data.microphone) talk='microphone_disabled';
  else if(busy || !['idle','speaking'].includes(data.app_state)) talk='busy';
  return {talk,speaker};
}
function nextStep(data) {
  if(!data.flipper_connected) return '先启用 Flipper 的 AI session';
  if(data.wifi_pending) return '等待新 Wi-Fi 验证，原配置保留';
  if(data.wifi!=='connected') return '连接家庭 Wi-Fi';
  if(!data.config_ready) return '导入或保存服务器连接配置';
  if(data.applying) return '等待配置应用完成';
  if(data.server_state==='auth_failed') return '在服务端重新签发并导入 Token';
  if(data.server_state==='tls_failed') return '检查服务器证书、域名和设备时间';
  if(['connect_failed','connect_timeout','dns_failed'].includes(data.server_state)) return '检查地址、端口和服务端运行状态';
  if(data.app_state==='listening') return '说完后点击结束录音并发送';
  if(data.app_state==='speaking') return '等待语音回复播放完成';
  if(data.server_state!=='authenticated') return '测试连接与设备认证';
  return '测试提示音，再开始对话';
}
function uartState(data) {
  if(data.flipper_connected) return '已连接';
  const stages={no_heartbeat:'未收到 Flipper 发现心跳，请核对 GPIO17/18',baud_ack_failed:'波特率协商未确认',rpc_start_failed:'RPC 启动未确认',hello_rejected:'助手会话未获确认',discovery:'等待设备发现',hello:'正在建立会话',link_lost:'连接已中断'};
  const reasons={frame_checksum:'帧校验失败',frame_type:'帧字节错位',frame_length:'帧长度错误',rpc_decode:'RPC 数据解析失败',rpc_response_timeout:'RPC 回复超时',data_ack_timeout_or_rejected:'数据确认失败',invalid_session:'会话失效',heartbeat_write_failed:'心跳发送失败',rpc_stream_overflow:'RPC 接收缓冲超限'};
  Object.assign(reasons,{native_state_unconfirmed:'工具执行状态未确认，已重建连接',native_response_invalid:'工具回复超限或无效',native_status_rejected:'工具调用前会话校验失败',native_ack_timeout:'工具请求确认超时',native_cancel_or_deadline:'工具取消或执行超时，已重建连接',native_response_timeout:'工具回复超时，已重建连接'});
  const reason=data.uart?.last_disconnect&&data.uart.last_disconnect!=='none'?data.uart.last_disconnect:data.uart?.previous_disconnect;
  return '未连接 · '+(stages[data.uart?.stage]||data.uart?.stage||'检查 AI session 与接线')+(reason&&reason!=='none'?'（上次中断：'+(reasons[reason]||reason)+'）':'');
}
function voiceControl(data) {
  // Recording must offer SEND, even though starting another recording is busy.
  if(data.app_state==='listening') return {path:'stop-talk',label:'发送这段录音',disabled:false};
  return {path:'talk',label:data.app_state==='connecting'?'正在连接…':data.app_state==='speaking'?'打断并开始录音':'开始录音',disabled:Boolean(actionState(data).talk)};
}
if(typeof module!=='undefined' && module.exports) module.exports={DraftBook,wifiOutcome,actionState,nextStep,uartState,voiceControl};
if(typeof document==='undefined') return;
const $=id=>document.getElementById(id), book=new DraftBook();
let csrf='', timer=null, epoch=0, loadedRevision=-1, live=null, polling=false, writing=false, expectedWifi=null, expectedProbe=false, lost=false;
let preview=null;
const fields={wifi:{ssid:'ssid',wifi_password:'wifi-password'},server:{url:'url',token:'token',clear_token:'clear-token'},audio:{volume:'volume',led_brightness:'led',mute:'mute',microphone:'microphone'}};
const labels={starting:'启动中',setup:'等待配网',connecting:'连接中',connected:'已连接',getting_ip:'已关联，等待 IP',failed:'连接失败',not_tested:'尚未测试',authenticated:'最近认证通过',auth_failed:'设备 Token 被拒绝',needs_config:'请先配置地址和 Token',wifi_offline:'Wi-Fi 未连接',dns_failed:'DNS 解析失败',tls_failed:'TLS 连接失败',connect_timeout:'服务器连接超时',connect_failed:'无法连接服务器',hello_timeout:'未收到服务器确认',invalid_hello:'服务器响应与语音协议不匹配',busy:'设备忙，请稍后重试',idle:'待机',listening:'正在录音',speaking:'正在播放',activating:'正在准备语音'};
const errors={login_required:'登录已过期，请重新登录。',invalid_password:'管理密码不正确。',try_later:'尝试过多，请稍后再试。',busy:'设备正在处理其他操作，请稍后重试。',revision_conflict:'设置已变化，输入已保留。点击“刷新并保留输入”后再保存。',wrong_device_or_format:'连接配置不属于这块设备，或格式不正确。',invalid_connection_fields:'配置中的服务器地址或 Token 不正确。',invalid_url:'请填写设备可访问的 WS/WSS 地址，不能填 localhost。',invalid_token:'Token 不能含空格或换行，最多 2048 字节。',invalid_wifi_fields:'Wi-Fi 名称最多 32 字节，密码最多 64 字节。',invalid_wifi_password:'密码为 8–63 字节或 64 位十六进制密钥；开放网络请留空。',wifi_offline:'请先连接 Wi-Fi。',module_offline:'请先启用 Flipper AI session，并确认串口已连接。',needs_config:'请先保存或导入服务器地址和设备 Token。',microphone_disabled:'麦克风已关闭，请先启用并保存。',speaker_muted:'当前已静音，请先取消静音并保存。',volume_zero:'当前音量为 0，请先调高音量并保存。',not_listening:'当前没有录音，录音可能已自动结束。',password_length_8_64:'管理密码长度应为 8–64 字节。'};
function message(text,error=false) { $('message').hidden=false;$('message').textContent=text;$('message').classList.toggle('error',error); }
function loginView() { preview?.reset();epoch++;clearInterval(timer);csrf='';$('console').hidden=true;$('logout').hidden=true;$('login').hidden=false;for(const id of ['password','token','wifi-password','new-password','connection-file']) $(id).value=''; }
async function api(path,body,options={}) {
  const generation=epoch, controller=new AbortController(), timeout=setTimeout(()=>controller.abort(),8000);
  const abort=()=>controller.abort();
  if(options.signal?.aborted)controller.abort();else options.signal?.addEventListener('abort',abort,{once:true});
  try {
    const response=await fetch(path,{method:body===undefined?'GET':'POST',credentials:'same-origin',signal:controller.signal,headers:body===undefined?{}:{'Content-Type':options.raw?'application/octet-stream':'application/json','X-CSRF-Token':csrf,...options.headers},body:body===undefined?undefined:options.raw?body:JSON.stringify(body)});
    const result=await response.json();
    if(generation!==epoch) throw Object.assign(new Error('stale response'),{stale:true});
    if(!response.ok) {
      if(response.status===401 && path!=='/api/login') {const expired=Boolean(csrf);loginView();if(expired)message(errors.login_required,true);}
      throw Object.assign(new Error(errors[result.error]||result.error||'请求失败'),{status:response.status,code:result.error});
    }
    return result;
  } catch(error) {
    if(!error.status && !error.stale) error.network=true;
    throw error;
  } finally { clearTimeout(timeout);options.signal?.removeEventListener('abort',abort); }
}
function refreshActions() {
  if(!live) return;
  const state=actionState(live);
  const voice=voiceControl(live);
  $('talk').disabled=writing||voice.disabled;$('talk').textContent=voice.label;
  $('talk').title=voice.path==='talk'?(errors[state.talk]||''):'';
  $('test-speaker').disabled=writing||Boolean(state.speaker);$('test-speaker').title=errors[state.speaker]||'';
  $('test-server').disabled=writing||live.probing||live.wifi_pending||live.applying||live.audio_preview?.busy||live.wifi!=='connected'||!live.config_ready;
  $('wifi-save').disabled=writing||live.wifi_pending||live.probing||live.applying||live.audio_preview?.busy;
  $('close-hotspot').disabled=writing||!live.ap||live.wifi!=='connected';
  $('voice-hint').textContent=live.app_state==='listening'?'正在录音。说完后点击“发送这段录音”，等待语音回复。':live.app_state==='connecting'?'正在连接语音服务，请等按钮切换为“发送这段录音”后再说话。':(state.talk?errors[state.talk]:'点击开始录音，说完后再次点击同一个按钮发送。Flipper 桌面也可短按 OK 录音／发送，长按 OK 打开菜单。');
  preview?.refresh(live,writing);
}
async function busy(button,action,mutation=true) {
  if(mutation&&writing) {message('正在处理上一项操作，请稍后重试。',true);return;}
  if(mutation) writing=true;
  button.disabled=true;
  try {await action();} catch(error) {if(!error.stale) message(error.network?'暂时无法访问设备，请按 Flipper 显示的 IP 恢复连接。':error.message,true);}
  finally {if(mutation) writing=false;button.disabled=false;refreshActions();}
}
function download(name,data) {const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'}));const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
function applySettings(data,options={}) {
  const apply=book.merge(data,options);loadedRevision=data.revision;
  if(apply.includes('wifi')) {$('ssid').value=data.saved_ssid||'';$('wifi-password').value='';}
  if(apply.includes('server')) {$('url').value=data.url||'';$('token').value='';$('clear-token').checked=false;}
  if(apply.includes('audio')) {$('volume').value=data.volume;$('led').value=data.led_brightness;$('mute').checked=data.mute;$('microphone').checked=data.microphone;}
  $('token-state').textContent=data.token_set?'已保存 · 不回显':'尚未设置';
  $('identity').textContent=`Device ID: ${data.device_id}\nClient ID: ${data.client_id}`;
  render(data);
}
async function load(options={}) {applySettings(await api('/api/settings'),options);}
function render(data) {
  live=data;
  if($('audio-stream-diagnostics')) $('audio-stream-diagnostics').textContent=data.audio_stream?JSON.stringify(data.audio_stream,null,2):'请更新 ESP32 固件以查看流式音频诊断。';
  if($('audio-stream-summary')) {
    const a=data.audio_stream;
    $('audio-stream-summary').textContent=!a?.turn?'完成一次对话后显示本轮统计。':`第 ${a.turn} 轮：${a.source_rate||'—'} Hz / ${a.frame_ms||'—'} ms，收到 ${a.packets} 包，状态丢弃 ${a.state_drops}，队列丢弃 ${a.queue_drops}，解码错误 ${a.decode_errors}，重采样错误 ${a.resample_errors}。最大来包间隔 ${a.rx_gap_max_ms} ms，最长解码 ${a.decode_max_ms} ms，本轮最大输出等待 ${a.output_gap_max_ms} ms；断粮后重新缓冲 ${a.starvation_events??'—'} 次，最长额外等待 ${a.starvation_max_ms??'—'} ms；本轮输出约 ${(a.played_ms/1000).toFixed(1)} 秒。`;
  }
  $('flipper-state').textContent=uartState(data);
  if($('uart-diagnostics')) $('uart-diagnostics').textContent=data.uart?JSON.stringify(data.uart,null,2):'当前 ESP32 固件尚未提供诊断信息，请更新 ESP32 固件。';
  $('wifi-state').textContent=(labels[data.wifi]||data.wifi)+(data.ssid?' · '+data.ssid:'');
  $('ip-state').textContent=data.ip?'http://'+data.ip:data.ap?'http://'+data.ap_ip:'等待获取 IP';
  $('server-state').textContent=(data.probing?'正在测试…':labels[data.server_state]||data.server_state)+(data.server_check_age_s>=0?` · ${data.server_check_age_s} 秒前`:'');
  $('next-step').textContent=nextStep(data);
  $('voice-state').textContent=data.applying?'正在应用配置':labels[data.app_state]||data.app_state||'等待设备状态';
  const note=$('network-note');note.replaceChildren();
  if(data.wifi_error) note.append(document.createTextNode('网络反馈：'+wifiReason(data.wifi_error)+' '));
  if(data.ip) {
    note.append(document.createTextNode('局域网设置页：'));
    const link=document.createElement('a');link.href='http://'+data.ip;link.textContent=link.href;link.target='_blank';link.rel='noopener';note.append(link);
    if(data.ap&&data.ap_close_in_s>=0) note.append(document.createTextNode(`。热点约 ${data.ap_close_in_s} 秒后关闭；请切回家庭 Wi-Fi，新地址需要重新登录。`));
    else if(data.ap) note.append(document.createTextNode('。设置热点也保持开启，可手动关闭。'));
  } else if(data.ap) note.append(document.createTextNode(`设置热点 ${data.ap_ssid} 已开启，地址 http://${data.ap_ip}。`));
  refreshActions();
}
async function poll() {
  if(polling||!csrf) return;
  polling=true;
  try {
    const data=await api('/api/status');render(data);
    const outcome=wifiOutcome(data.wifi_trial,expectedWifi);
    if(outcome) {
      const request=expectedWifi;expectedWifi=null;
      if(outcome==='saved') {
        await load({ack:request.ticket,own:{before:data.wifi_trial.before_revision,after:data.wifi_trial.revision}});
        message('新 Wi-Fi 已确认连接并保存。切回家庭 Wi-Fi，按新 IP 继续访问；其他未保存输入已保留。');
      } else message(`新网络“${data.wifi_trial.ssid}”未保存，原配置仍保留。${wifiReason(data.wifi_trial.error)}`,true);
    } else if(data.revision!==loadedRevision) await load();
    if(expectedProbe&&!data.probing&&data.server_state!=='connecting'){expectedProbe=false;message(data.server_state==='authenticated'?'服务器认证通过。请继续测试提示音和实际对话。':(labels[data.server_state]||data.server_state),data.server_state!=='authenticated');}
    if(lost&&!outcome) message('已恢复与设备连接。未确认的操作不会自动重发。');
    lost=false;
  } catch(error) {
    if(!error.stale && error.status!==401) {lost=true;message(error.network?'设备暂时不可达。网络切换时请按 Flipper 显示的新 IP 重新打开。':error.message,true);}
  } finally {polling=false;}
}
async function open() {$('login').hidden=true;$('console').hidden=false;$('logout').hidden=false;await load();clearInterval(timer);timer=setInterval(poll,2000);}
for(const [group,properties] of Object.entries(fields)) for(const [field,id] of Object.entries(properties)) $(id).addEventListener('input',()=>book.touch(group,field));
$('login-form').addEventListener('submit',event=>{event.preventDefault();busy(event.submitter,async()=>{epoch++;csrf=(await api('/api/login',{password:$('password').value})).csrf;$('password').value='';await open();$('message').hidden=true;});});
$('logout').onclick=()=>busy($('logout'),async()=>{await api('/api/logout',{});loginView();});
$('refresh').onclick=()=>busy($('refresh'),async()=>{await load({rebase:true});message('已刷新已保存设置，未提交输入已保留，可继续保存。');},false);
$('wifi-form').addEventListener('submit',event=>{event.preventDefault();busy(event.submitter,async()=>{
  const ticket=book.ticket('wifi'),request_id=Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);
  expectedWifi={request_id,ticket};
  const body={revision:ticket.revision,request_id,ssid:$('ssid').value,wifi_password:$('wifi-password').value};
  try {await api('/api/wifi',body);$('wifi-password').value='';message('正在测试新网络；只有拿到 IP 才会保存，请等待最终结果。');}
  catch(error) {if(!error.network){expectedWifi=null;throw error;}message('配网请求结果待确认。恢复连接后会查询结果，不会重复提交。',true);}
});});
$('scan').onclick=()=>busy($('scan'),async()=>{
  await api('/api/scan',{});message('正在扫描 2.4 GHz 网络…');let data;
  for(let i=0;i<25;i++){await new Promise(resolve=>setTimeout(resolve,400));data=await api('/api/scan');if(!data.scanning)break;}
  $('networks').replaceChildren();
  for(const network of data.networks){const button=document.createElement('button');button.type='button';button.textContent=`${network.ssid} · ${network.rssi} dBm${network.secure?' · 加密':''}`;button.onclick=()=>{$('ssid').value=network.ssid;$('wifi-password').value='';book.touch('wifi','ssid');book.touch('wifi','wifi_password');};$('networks').append(button);}
  message(data.scanning?'扫描尚未完成，请稍后重试。':data.networks.length?`发现 ${data.networks.length} 个网络，请选择。`:'未发现网络，可手动填写名称。');
},false);
for(const group of ['server','audio']) $(group+'-form').addEventListener('submit',event=>{event.preventDefault();busy(event.submitter,async()=>{
  const ticket=book.ticket(group);if(!ticket.fields.length){message('没有需要保存的更改。');return;}
  const body={revision:ticket.revision};
  for(const key of ticket.fields){const element=$(fields[group][key]);body[key]=element.type==='checkbox'?element.checked:element.type==='number'?Number(element.value):element.value.trim();}
  const data=await api('/api/settings',body);applySettings(data,{ack:ticket,own:{before:ticket.revision,after:data.revision}});
  message(group==='server'?'服务器配置已保存，请测试连接与设备认证。':'声音和灯光已保存，其他未提交输入已保留。');
});});
$('export-identity').onclick=()=>busy($('export-identity'),async()=>download('deepflipper-identity.json',await api('/api/identity')),false);
$('connection-file').onchange=()=>busy($('connection-file'),async()=>{
  try {const ticket=book.ticket('server');const file=$('connection-file').files[0];if(!file)return;if(file.size>8192)throw new Error('配置文件过大。');const data=JSON.parse((await file.text()).replace(/^\uFEFF/,''));if(!data||typeof data!=='object'||Array.isArray(data))throw new Error('配置格式不正确。');if(data.expires_at&&Date.now()/1000>data.expires_at)throw new Error('连接配置已过期，请在服务端重新生成。');const saved=await api('/api/import',{...data,revision:ticket.revision});applySettings(saved,{ack:ticket,own:{before:ticket.revision,after:saved.revision}});message('已导入本设备的连接配置，请测试认证。');}
  finally {$('connection-file').value='';}
}).finally(()=>{$('connection-file').value='';});
$('password-form').addEventListener('submit',event=>{event.preventDefault();busy(event.submitter,async()=>{await api('/api/password',{password:$('new-password').value});loginView();message('管理密码已修改，请重新登录。');});});
$('talk').onclick=()=>busy($('talk'),async()=>{
  const control=voiceControl(live||{});
  if(control.disabled) return;
  await api('/api/'+control.path,{});
  message(control.path==='stop-talk'?'已请求结束录音并发送，正在等待语音回复。':'正在连接，请等待显示“正在录音”后说话。');
  await poll();
});
for(const [id,path,text] of [['reconnect','reconnect','已请求重连。网络可能短暂断开，必要时按 Flipper 的 IP 重新访问。'],['hotspot','hotspot','已请求开启设置热点。'],['close-hotspot','hotspot/close','热点即将关闭，请切回家庭 Wi-Fi 并打开局域网设置地址。'],['test-server','test-server','正在测试认证，请查看服务器检查结果。'],['test-speaker','test-speaker','已请求播放提示音，请以实际听到声音为准。']]) $(id).onclick=()=>busy($(id),async()=>{await api('/api/'+path,{});if(id==='test-server')expectedProbe=true;message(text);});
preview=new DfAudioPreview.Controller({get:$,api,message,onStatus:poll});
Object.assign(errors,{module_standby:'ESP32 处于省电待机，请短按 Flipper OK 或 ESP32 BOOT 唤醒。',invalid_audio_chunk:'音频分块顺序或长度不正确，请重新上传。',invalid_audio_format:'仅支持 30 秒以内的 24 kHz 单声道音频。',preview_busy_or_no_memory:'设备忙或内存不足，请停止当前测试后再试。',audio_upload_incomplete:'音频上传未完成，请重新上传。',audio_id_mismatch:'试听任务已改变，请刷新设备状态。'});
(async()=>{try{csrf=(await api('/api/session')).csrf;await open();}catch{loginView();}})();
})();
