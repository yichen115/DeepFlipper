'use strict';
const $ = id => document.getElementById(id);
let csrf = '', revision = '', pollTimer = null, connectionProfile = null;
let historyCursor = 0, historyNext = 0, historyLoading = false, historyGeneration = 0;
let historyClearRequest = null;
let previewUrl = null;
let previewGeneration = 0;
let streamTesting = false;
function refreshTts() {
  const local=$('tts-provider').value==='LocalTTS';
  $('tts-speed').disabled=!local;
  $('tts-voice').disabled=local;
  $('tts-hint').textContent=local?'使用本机 CPU 合成中文／英文，不访问微软接口。首次请运行 python scripts/download_models.py --tts-only。':'EdgeTTS 需要服务器能够连接微软语音服务；网络不可用时建议切换本地合成。';
}
function message(text, error = false) {
  const box = $('message'); box.textContent = text; box.className = error ? 'error' : ''; box.hidden = false;
  clearTimeout(message.timer); message.timer = setTimeout(() => { box.hidden = true; }, 6500);
}
async function api(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  if (options.body !== undefined) {
    headers['Content-Type'] = 'application/json'; headers['X-CSRF-Token'] = csrf;
    options.body = JSON.stringify(options.body);
  }
  const response = await fetch(path, { ...options, headers, credentials: 'same-origin' });
  const raw = await response.text();
  let data; try { data = JSON.parse(raw); } catch { data = { error: raw || '请求失败' }; }
  if (!response.ok) {
    if (response.status === 401 && path !== '/api/login') showLogin();
    throw new Error(data.error || `请求失败 (${response.status})`);
  }
  return data;
}
function showLogin() {
  previewGeneration++;
  if(previewUrl) URL.revokeObjectURL(previewUrl);previewUrl=null;
  $('tts-preview').pause();$('tts-preview').removeAttribute('src');$('tts-preview').hidden=true;
  $('download-tts').hidden=true;$('download-tts').removeAttribute('href');
  historyGeneration++;historyCursor=historyNext=0;
  historyLoading=false;historyClearRequest=null;historyControls(false);
  $('history-events').replaceChildren();
  connectionProfile = null;
  $('console').hidden = true; $('login-card').hidden = false; $('runtime').textContent = '未登录';
  clearInterval(pollTimer); pollTimer = null; csrf = ''; $('device-token').value = ''; $('api-key').value = '';
  revision = ''; $('issued').hidden = true; $('expires').textContent = '';
  for (const id of ['base-url', 'model-name', 'prompt', 'tts-voice', 'voice-url', 'device-id', 'client-id']) $(id).value = '';
  $('tools').replaceChildren(); $('devices').replaceChildren(); $('tool-policy').textContent = '';
}
function draft() {
  return { revision, base_url: $('base-url').value.trim(), model_name: $('model-name').value.trim(),
    api_key: $('api-key').value.trim(), clear_api_key: $('clear-key').checked,
    prompt: $('prompt').value, context_turns:Number($('context-turns').value),tts_voice: $('tts-voice').value.trim(),
    tts_speed: Number($('tts-speed').value || 1.0), tts_provider:$('tts-provider').value, tool_calling: $('tool-calling').checked };
}
function invalidateConnection() {
  if(connectionProfile) message('设备信息已改变，旧连接配置已收起，请重新生成。');
  connectionProfile = null; $('issued').hidden = true; $('device-token').value = '';
}
async function loadSettings() {
  invalidateConnection();
  const data = await api('/api/settings'); revision = data.revision;
  for (const [id, value] of [['base-url', data.base_url], ['model-name', data.model_name],
    ['prompt', data.prompt], ['tts-voice', data.tts_voice], ['voice-url', data.websocket]]) $(id).value = value || '';
  $('tts-speed').value = String(data.tts_speed ?? 1.0);
  $('key-state').textContent = data.api_key_set ? '已保存 · 不回显' : '尚未配置';
  $('api-key').value = ''; $('clear-key').checked = false;
  $('tool-calling').checked = data.tool_calling;
  $('asr-name').textContent = data.asr; $('tts-name').textContent = data.tts;
  $('tts-provider').value=data.tts_provider||data.tts||'EdgeTTS';refreshTts();
  $('context-turns').value=data.context_turns??6;
  $('tool-policy').textContent = data.tool_policy; $('tools').replaceChildren();
  for (const item of data.tools) {
    const row = document.createElement('div'); row.className = 'tool';
    for (const [tag, text] of [['code', item.name], ['p', item.description], ['small', item.parameters]]) {
      const node = document.createElement(tag); node.textContent = text; row.append(node);
    }
    $('tools').append(row);
  }
}
async function status() {
  const data = await api('/api/status');
  if ($('console').hidden) return;
  const labels = { starting: '启动中', applying: '正在应用设置', needs_config: '请先配置模型', ready: '语音服务就绪', error: '语音服务未就绪' };
  const error = data.error === 'missing_speech_models' ? '请先运行 download_models.py 下载语音模型' : data.error;
  $('runtime').textContent = (labels[data.state] || data.state) + (error ? ` · ${error}` : '');
  $('runtime').className = `badge ${data.state === 'ready' ? 'ready' : data.state === 'error' ? 'error' : ''}`;
  $('devices').replaceChildren();
  if (!data.devices.length) $('devices').textContent = '暂无语音连接。可先在 ESP32 网页下载设备身份，在这里导入并生成连接配置。';
  for (const item of data.devices) {
    const row = document.createElement('div'); row.className = 'device';
    const id = document.createElement('div'); id.textContent = `Device ID: ${item.device_id}`; row.append(id);
    const client = document.createElement('div'); client.textContent = `Client ID: ${item.client_id}`; row.append(client);
    const tools = document.createElement('div'); tools.textContent = `设备上报工具：${item.tools.join(', ') || '等待 MCP 协商'}`; row.append(tools);
    if(item.audio_tx){const a=document.createElement('div');a.textContent=`本轮语音发送：${item.audio_tx.sample_rate} Hz，已发 ${item.audio_tx.packets} 包，队列 ${item.audio_tx.queued} 包，最大发送间隔 ${item.audio_tx.max_send_gap_ms} ms，长停顿后重新预填充 ${item.audio_tx.refill_bursts??0} 次。`;row.append(a);}
    const use = document.createElement('button'); use.className = 'secondary'; use.type = 'button'; use.textContent = '使用这组身份';
    use.addEventListener('click', () => { $('device-id').value = item.device_id; $('client-id').value = item.client_id; invalidateConnection(); }); row.append(use);
    for(const segmented of [false,true]) {
    const test=document.createElement('button');test.className='secondary';test.type='button';test.textContent=segmented?'按真实对话分句测试':'播放长语音测试';test.disabled=streamTesting;
    test.addEventListener('click',()=>busy(test,async()=>{
      streamTesting=true;
      try {
        message(segmented?'正在使用真实 TTS 分句队列测试，不调用大模型和工具…':'正在向设备发送固定长语音，不调用大模型和工具…');
        const result=await api('/api/tts-stream-test',{method:'POST',body:{device_id:item.device_id,segmented}});
        message(`测试语音已发送，约 ${(result.duration_ms/1000).toFixed(1)} 秒；可下载同一段样本到 ESP32 网页做缓冲播放对照。`);
      } finally {streamTesting=false;}
    }));row.append(test);
    }
    const sample=document.createElement('a');sample.href='/api/tts-stream-test/audio';sample.textContent='下载最近一次测试的音频样本';row.append(sample);
    $('devices').append(row);
  }
  if(!$('history-panel').hidden && !historyCursor) await loadHistory();
}
async function loadHistory(before=0) {
  if(historyLoading || historyClearRequest) return;
  const request={};historyLoading=request;
  const generation=historyGeneration, device=$('history-device').value;
  try {
    const data=await api('/api/history?device='+encodeURIComponent(device)+'&before='+before);
    if(generation!==historyGeneration || device!==$('history-device').value || $('console').hidden) return;
    historyCursor=before;historyNext=data.before;
    const options=[...$('history-device').options].map(o=>o.value);
    for(const value of data.devices) if(!options.includes(value)) {
      const option=document.createElement('option');option.value=value;option.textContent=value||'未标识设备';$('history-device').append(option);
    }
    $('history-events').replaceChildren();
    for(const event of data.events) {
      const row=document.createElement('article');row.className='history-event '+event.kind;
      const meta=document.createElement('small');meta.textContent=`${new Date(event.time*1000).toLocaleString()} · ${event.device} · ${{user:'你',assistant:'鲸鱼娘',status:'状态',error:'错误'}[event.kind]||event.kind}`;
      const text=document.createElement('div');text.textContent=event.text;row.append(meta,text);$('history-events').append(row);
    }
    $('history-note').textContent=data.write_error?'记录写入出现异常，请检查服务端数据目录和磁盘。':data.events.length?(before?'正在查看较早记录，点击“查看最新”返回。':'显示最近记录，每 2.5 秒刷新。'):'暂无记录。开始一次对话后，这里会显示识别文字、回复和处理状态。';
    $('history-older').disabled=!historyNext;
  } finally {if(historyLoading===request)historyLoading=false;}
}
function historyControls(disabled) {
  for(const id of ['history-device','history-refresh','history-clear']) $(id).disabled=disabled;
  $('history-older').disabled=disabled || !historyNext;
  $('history-clear').textContent=$('history-device').value?'清空该设备记录':'清空全部记录';
}
async function clearHistory() {
  if(historyClearRequest || $('console').hidden) return;
  const device=$('history-device').value;
  const scope=device?'device':'all';
  if(!confirm(`确定清空${device?'设备 '+device:'全部设备'}的全部聊天记录和上下文吗？对应设备当前的语音会话会结束，此操作无法撤销。`)) return;
  const request={};historyClearRequest=request;
  const generation=++historyGeneration;
  historyLoading=false;historyControls(true);
  $('history-note').textContent='正在清空记录并重置对应语音会话…';
  try {
    const result=await api('/api/history/clear',{method:'POST',body:{scope,device}});
    if(generation!==historyGeneration || $('console').hidden) return;
    historyCursor=historyNext=0;
    $('history-events').replaceChildren();
    message(`已清空 ${result.deleted} 条记录并重置上下文。`+(result.reset_pending?'部分语音连接尚未完成断开，请在设备上结束当前对话后重新开始。':'下一次对话将从新的上下文开始。'),!!result.reset_pending);
  } catch(error) {
    if(generation===historyGeneration && !$('console').hidden)message(error.message,true);
  } finally {
    if(historyClearRequest===request) {
      historyClearRequest=null;historyControls(false);
      if(generation===historyGeneration && !$('console').hidden)await loadHistory(historyCursor);
    }
  }
}
async function openConsole() {
  await loadSettings(); $('login-card').hidden = true; $('console').hidden = false;
  await status(); clearInterval(pollTimer); pollTimer = setInterval(() => { status().catch(() => {}); }, 2500);
}
async function busy(button, action) {
  button.disabled = true;
  try { await action(); } catch (error) { message(error.message, true); }
  finally { button.disabled = false; }
}
$('login-form').addEventListener('submit', event => {
  event.preventDefault(); busy(event.submitter, async () => {
    const result = await api('/api/login', { method: 'POST', body: { token: $('admin-token').value.trim() } });
    csrf = result.csrf; $('admin-token').value = ''; await openConsole();
  });
});
$('logout').addEventListener('click', () => busy($('logout'), async () => { await api('/api/logout', { method: 'POST', body: {} }); showLogin(); }));
document.querySelectorAll('.tab').forEach(button => button.addEventListener('click', () => {
  document.querySelectorAll('.tab').forEach(tab => tab.classList.toggle('active', tab === button));
  document.querySelectorAll('.panel').forEach(panel => { panel.hidden = panel.id !== button.dataset.panel; });
  document.querySelector('.save-bar').hidden = ['device-panel','history-panel'].includes(button.dataset.panel);
  if(button.dataset.panel==='history-panel') loadHistory(historyCursor).catch(error=>message(error.message,true));
}));
$('history-device').addEventListener('change',()=>{historyGeneration++;historyLoading=false;historyCursor=historyNext=0;historyControls(false);loadHistory().catch(error=>message(error.message,true));});
$('history-clear').addEventListener('click',()=>clearHistory().catch(error=>message(error.message,true)));
$('history-refresh').addEventListener('click',()=>busy($('history-refresh'),()=>loadHistory()));
$('history-older').addEventListener('click',()=>busy($('history-older'),()=>loadHistory(historyNext)));
$('save').addEventListener('click', () => busy($('save'), async () => {
  const result = await api('/api/settings', { method: 'POST', body: draft() }); revision = result.revision;
  await loadSettings(); await status(); message('已保存，语音服务正在重新应用设置。');
}));
$('test-model').addEventListener('click', () => busy($('test-model'), async () => {
  const result = await api('/api/model-test', { method: 'POST', body: draft() });
  message(result.tool_call_returned ? '模型接口与工具调用测试通过。' : '接口可达，但未返回工具调用；请确认模型支持 tools。', !result.tool_call_returned);
}));
$('tts-provider').addEventListener('change',refreshTts);
async function testTts(mode) {
  const generation=++previewGeneration;
  message('正在生成试听音频…');
  const response=await fetch('/api/tts-test',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify({tts_provider:$('tts-provider').value,tts_speed:Number($('tts-speed').value||1.0),preview_mode:mode})});
  if(!response.ok){if(response.status===401)showLogin();const text=await response.text();let error;try{error=JSON.parse(text).error;}catch{error=text;}throw new Error(error||'试听失败');}
  const audio=await response.blob();
  if($('console').hidden || generation!==previewGeneration)return;
  if(previewUrl)URL.revokeObjectURL(previewUrl);
  previewUrl=URL.createObjectURL(audio);$('tts-preview').src=previewUrl;$('tts-preview').hidden=false;
  $('download-tts').href=previewUrl;$('download-tts').download=`tts-preview-${mode}.${audio.type==='audio/wav'?'wav':'mp3'}`;$('download-tts').hidden=false;
  await $('tts-preview').play().catch(()=>{});message('试听已生成，可点击播放器播放。切换语音方式后请保存并应用。');
}
$('test-tts').addEventListener('click',()=>busy($('test-tts'),()=>testTts('source')));
$('test-tts-opus').addEventListener('click',()=>busy($('test-tts-opus'),()=>testTts('opus')));
$('device-form').addEventListener('submit', event => {
  event.preventDefault(); busy(event.submitter, async () => {
    invalidateConnection();
    const request = { device_id: $('device-id').value.trim(), client_id: $('client-id').value.trim(), server_url: $('voice-url').value.trim() };
    const result = await api('/api/connection-profile', { method: 'POST', body: request });
    if ($('device-id').value.trim() !== result.device_id || $('client-id').value.trim() !== result.client_id || $('voice-url').value.trim() !== result.server_url) { message('设备信息已改变，请重新生成连接配置。', true); return; }
    connectionProfile = result;
    $('device-token').value = result.token; $('expires').textContent = `有效期约 ${Math.round(result.expires_in / 86400)} 天。下载连接配置后，在 ESP32 网页导入；文件包含设备凭据，请勿提交仓库。`;
    $('issued').hidden = false; message('设备凭据已生成，仅在本次页面中显示。');
  });
});
$('copy-token').addEventListener('click', () => busy($('copy-token'), async () => {
  if (navigator.clipboard?.writeText) { await navigator.clipboard.writeText($('device-token').value); message('已复制设备 Token。'); }
  else { $('device-token').select(); message(document.execCommand('copy') ? '已复制设备 Token。' : '已选中 Token，请按 Ctrl+C，或下载连接配置文件。'); }
}));
(async () => { try { const session = await api('/api/session'); csrf = session.csrf; await openConsole(); } catch { showLogin(); } })();

$('identity-file').addEventListener('change', () => busy($('identity-file'), async () => {
  const file = $('identity-file').files[0]; if (!file) return;
  if (file.size > 4096) throw new Error('设备身份文件过大。');
  const data = JSON.parse(await file.text());
  if (!data || data.format !== 'deepflipper.identity' || data.version !== 1 ||
      typeof data.device_id !== 'string' || typeof data.client_id !== 'string') throw new Error('不是有效的 DeepFlipper 设备身份文件。');
  $('device-id').value = data.device_id; $('client-id').value = data.client_id;
  $('identity-file').value = ''; $('issued').hidden = true; connectionProfile = null;
  message('已导入设备身份。确认服务器地址后生成连接配置。');
}));
$('download-connection').addEventListener('click', () => {
  if (!connectionProfile) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(connectionProfile, null, 2)], {type: 'application/json'}));
  const link = document.createElement('a'); link.href = url; link.download = 'deepflipper-connection.json'; link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000); message('已下载。请在 ESP32 网页导入此配置文件。');
});

for (const id of ['device-id', 'client-id', 'voice-url']) $(id).addEventListener('input', invalidateConnection);
