'use strict';
const $ = id => document.getElementById(id);
let csrf = '', revision = '', pollTimer = null, connectionProfile = null;
let historyCursor = 0, historyNext = 0, historyLoading = false, historyGeneration = 0;
let historyClearRequest = null;
let previewUrl = null;
let previewGeneration = 0;
let streamTesting = false;
let breezeKeySet = false;
let screenGeneration = 0, screenLoading = false;
function refreshTts() {
  const provider=$('tts-provider').value, local=provider==='LocalTTS', breeze=provider==='BreezeTTS';
  $('tts-speed').disabled=!local;
  $('tts-voice').disabled=provider!=='EdgeTTS';
  $('breeze-voice').disabled=!breeze;
  $('tts-hint').textContent=local?'使用本机 CPU 合成中文／英文，不访问微软接口。首次请运行 python scripts/download_models.py --tts-only。'
    :breeze?(breezeKeySet?'BreezeBlue 云端合成（大肥鱼音色），边合成边播放；云端失败时本轮自动改用本地 MeloTTS。':'尚未配置 BreezeBlue API Key：写入 data/server.env 的 BREEZE_API_KEY 后重启服务。')
    :'EdgeTTS 需要服务器能够连接微软语音服务；网络不可用时建议切换本地合成。';
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
  screenGeneration++; showScreen(null);
}
function draft() {
  return { revision, base_url: $('base-url').value.trim(), model_name: $('model-name').value.trim(),
    api_key: $('api-key').value.trim(), clear_api_key: $('clear-key').checked,
    prompt: $('prompt').value, context_turns:Number($('context-turns').value),tts_voice: $('tts-voice').value.trim(),
    tts_speed: Number($('tts-speed').value || 1.0), tts_provider:$('tts-provider').value, breeze_voice:$('breeze-voice').value, tool_calling: $('tool-calling').checked,
    ui_confirm_dangerous: $('ui-confirm').checked, continuous_chat: $('continuous-chat').checked,
    continuous_follow_up_s: Number($('follow-up-s').value || 8) };
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
  $('ui-confirm').checked = data.ui_confirm_dangerous !== false;
  $('continuous-chat').checked = !!data.continuous_chat; $('follow-up-s').value = data.continuous_follow_up_s ?? 8;
  $('asr-name').textContent = data.asr; $('tts-name').textContent = data.tts;
  breezeKeySet=!!data.breeze_key_set;
  const bv=$('breeze-voice');
  if(data.breeze_voice && ![...bv.options].some(o=>o.value===data.breeze_voice)) bv.add(new Option(data.breeze_voice,data.breeze_voice));
  bv.value=data.breeze_voice||'clever';
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
  updateScreenDevices(data.devices);
  if(!$('device-panel').hidden) await loadScreen();
  if(!$('history-panel').hidden && !historyCursor) await loadHistory();
}
function updateScreenDevices(devices) {
  const select=$('screen-device'), current=select.value;
  const ids=[...new Set(devices.map(item=>item.device_id).filter(Boolean))];
  const existing=[...select.options].map(o=>o.value).filter(Boolean);
  if(ids.join('\n')===existing.join('\n')) return;
  select.replaceChildren();
  if(!ids.length) select.append(new Option('暂无语音连接',''));
  for(const id of ids) select.append(new Option(id,id));
  select.value=ids.includes(current)?current:(ids[0]||'');
  screenGeneration++; showScreen(null);
}
function showScreen(data) {
  const image=$('screen-image');
  if(data && data.png) { image.src='data:image/png;base64,'+data.png; image.hidden=false; $('screen-empty').hidden=true; }
  else { image.removeAttribute('src'); image.hidden=true; $('screen-empty').hidden=false;
    $('screen-empty').textContent=$('screen-device').value?'还没有截图。点“刷新屏幕”读取一帧（需要配套的 ESP32 固件）。':'设备建立语音连接后，这里显示 Flipper 屏幕。'; }
  const parts=[];
  if(data) {
    parts.push(data.app?`当前应用：${data.app}`:(data.time?'当前在主页':''));
    if(data.locked) parts.push('已锁屏');
    if(data.selected) parts.push(`选中：${data.selected}`);
    if(data.png_time) parts.push(`截图时间：${new Date(data.png_time*1000).toLocaleTimeString()}`);
  }
  $('screen-meta').textContent=parts.filter(Boolean).join(' · ');
  $('screen-view').textContent=data&&data.view?data.view:''; $('screen-view').hidden=!(data&&data.view);
}
async function loadScreen() {
  const device=$('screen-device').value;
  if(!device || screenLoading) return;
  const generation=screenGeneration; screenLoading=true;
  try {
    const data=await api('/api/flipper-screen?device='+encodeURIComponent(device));
    if(generation===screenGeneration && device===$('screen-device').value && !$('console').hidden) showScreen(data);
  } catch { /* 设备刚断开：等下一次状态刷新 */ }
  finally { screenLoading=false; }
}
$('screen-device').addEventListener('change',()=>{screenGeneration++;showScreen(null);loadScreen();});
$('screen-refresh').addEventListener('click',()=>busy($('screen-refresh'),async()=>{
  const device=$('screen-device').value;
  if(!device) throw new Error('请先让设备建立语音连接');
  const generation=++screenGeneration;
  const data=await api('/api/flipper-screen/refresh',{method:'POST',body:{device_id:device}});
  if(generation===screenGeneration && device===$('screen-device').value) showScreen(data);
  message('已读取 Flipper 屏幕。');
}));
const TOOL_LABELS = {
  flipper_app_open:'打开应用', flipper_app_exit:'退出应用', flipper_press_keys:'按键', flipper_press_button:'按键',
  flipper_ui_select:'选择', flipper_screen:'看屏幕', flipper_go_home:'回主页', flipper_nfc_read:'读 NFC',
  flipper_nfc_scan:'检测 NFC', flipper_infrared_learn:'学红外', flipper_subghz_record:'录射频', flipper_run_saved:'运行文件',
  flipper_app_call:'调用应用动作', flipper_app_describe:'查询应用动作', flipper_app_catalog:'列出应用',
  flipper_list_files:'浏览文件', flipper_list_apps:'列出应用', flipper_read_file:'读文件', flipper_alert:'提示音',
  sing_song:'点歌', list_songs:'查曲库', flipper_get_status:'查状态', flipper_system_info:'查设备信息',
};
const NOISY_STATUS = ['正在生成回复／语音','服务端语音发送结束','开始录音','收到录音发送请求，正在识别'];
let historyOpen = new Set();
function node(tag, className, text) {
  const item=document.createElement(tag); if(className) item.className=className;
  if(text!==undefined) item.textContent=text; return item;
}
function toolCard(event, data) {
  const row=node('article','history-event tool'); const args=data.args||{};
  const meta=node('small',undefined,`${new Date(event.time*1000).toLocaleTimeString()} · 操作`);
  const card=node('div','tool-card');
  const head=node('div','tool-head');
  const detail=args.name||args.text||args.keys||args.key||args.song||args.path||args.action||args.kind||args.url||'';
  head.append(node('b',undefined,TOOL_LABELS[data.name]||data.name||'工具'));
  if(detail) head.append(node('code',undefined,String(detail)));
  const state=data.code==='needs_confirmation'?['等待确认','warn']:data.ok===false||(data.code&&data.code!=='ok')?[data.code||'失败','bad']:['完成','good'];
  head.append(node('span','tool-state '+state[1],state[0]));
  card.append(head);
  if(data.app!==undefined || data.selected) card.append(node('small','tool-where',`${data.app?'应用 '+data.app:'主页'}${data.selected?' · 选中 '+data.selected:''}${data.action?' · '+data.action:''}`));
  if(data.image) {
    const img=node('img','tool-shot'); img.alt='当时的 Flipper 屏幕'; img.loading='lazy';
    img.src='/api/history/image?name='+encodeURIComponent(data.image); card.append(img);
  }
  const text=data.view||data.result;
  if(text) {
    const more=node('details'); const key=String(event.id); more.open=historyOpen.has(key);
    more.addEventListener('toggle',()=>{ if(more.open) historyOpen.add(key); else historyOpen.delete(key); });
    more.append(node('summary',undefined,data.view?'屏幕文字':'返回结果'), node('pre',undefined,text)); card.append(more);
  }
  row.append(meta,card); return row;
}
function renderHistory(events) {
  const box=$('history-events'); const verbose=!!$('history-verbose').checked;
  let session='';
  for(const event of events) {
    if(event.session && event.session!==session) {
      if(session) box.append(node('div','history-divider','新的语音会话 · '+new Date(event.time*1000).toLocaleString()));
      session=event.session;
    }
    if(event.kind==='status' && !verbose && NOISY_STATUS.includes(event.text)) continue;
    if(event.kind==='tool') {
      let data=null; try { data=JSON.parse(event.text); } catch { data=null; }
      if(data && typeof data==='object') { box.append(toolCard(event,data)); continue; }
    }
    const who={user:'你',assistant:'鲸鱼娘',status:'状态',error:'错误',tool:'操作'}[event.kind]||event.kind;
    const row=node('article','history-event '+event.kind);
    const meta=node('small',undefined,`${new Date(event.time*1000).toLocaleTimeString()} · ${who}${event.device&&!$('history-device').value?' · '+event.device:''}`);
    row.append(meta,node('div','bubble',event.text)); box.append(row);
  }
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
    renderHistory(data.events);
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
  document.querySelector('.save-bar').hidden = ['device-panel','history-panel','singing-panel'].includes(button.dataset.panel);
  if(button.dataset.panel==='history-panel') loadHistory(historyCursor).catch(error=>message(error.message,true));
  if(button.dataset.panel==='device-panel') loadScreen();
  if(button.dataset.panel==='singing-panel') loadSinging().catch(error=>message(error.message,true));
}));
let singingDownload = '', singingTimer = null;
function el(tag, text, className) { const node=document.createElement(tag); if(text!==undefined) node.textContent=text; if(className) node.className=className; return node; }
async function loadSinging() {
  const data = await api('/api/singing');
  const health = data.health || {};
  $('singing-status').textContent = !data.enabled ? '唱歌功能在语音服务配置里关闭了（singing.enabled: false）。'
    : !data.online ? (data.error || '连不上唱歌服务。')
    : `已连接 · ${health.gpu?.name || '未检测到显卡'} · 音色模型${health.model_ready ? '就绪' : '缺失'} · 排队 ${health.queue ?? 0} 首`
      + ` · 链接下载${health.yt_dlp ? '可用' : '未安装 yt-dlp'}` + (data.error ? ` · ${data.error}` : '');
  $('singing-status').className = 'note' + (data.online ? '' : ' error');
  const config = data.config;
  ['songs-dirs','download-dir','cache-dir','singing-save','song-url','song-title','song-download'].forEach(id => { $(id).disabled = !config; });
  if (config) {
    if (document.activeElement !== $('songs-dirs')) $('songs-dirs').value = config.songs_dirs.join('\n');
    if (document.activeElement !== $('download-dir')) $('download-dir').value = config.download_dir || '';
    if (document.activeElement !== $('cache-dir')) $('cache-dir').value = config.cache_dir || '';
  }
  const library = $('library'); library.replaceChildren();
  const songs = data.songs?.songs || [], renders = data.songs?.renders || [];
  const ready = new Set(renders.map(r => r.title));
  $('library-note').textContent = data.songs ? `原曲 ${songs.length} 首，练好的翻唱 ${renders.length} 份。` : '曲库读取失败。';
  songs.slice(0, 300).forEach(song => {
    const row = el('div', undefined, 'tool');
    row.append(el('code', song.title));
    if (ready.has(song.title)) row.append(el('span', ' 已练好', 'badge ready'));
    row.append(el('small', ` ${song.dir ? song.dir + ' · ' : ''}${song.file}`));
    library.append(row);
  });
}
async function pollSingingDownload() {
  clearTimeout(singingTimer);
  if (!singingDownload) return;
  const state = await api(`/api/singing/download?id=${singingDownload}`);
  const render = state.render || {};
  const stage = state.state === 'error' ? `下载失败：${state.error}`
    : state.state !== 'done' ? `下载中 ${Math.round((state.progress || 0) * 100)}%…`
    : render.state === 'done' ? `《${state.title}》已练好。` : render.state === 'error' ? `《${state.title}》没练成：${render.error || ''}`
    : `《${state.title}》已下载，正在练（${render.stage || render.state || '排队'} ${Math.round((render.progress || 0) * 100)}%）…`;
  $('download-state').textContent = stage;
  const finished = state.state === 'error' || (state.state === 'done' && ['done','error'].includes(render.state || 'done'));
  if (finished) { singingDownload = ''; loadSinging().catch(() => {}); return; }
  singingTimer = setTimeout(() => pollSingingDownload().catch(error => { $('download-state').textContent = error.message; }), 2000);
}
$('singing-refresh').addEventListener('click', () => busy($('singing-refresh'), () => loadSinging()));
$('singing-save').addEventListener('click', () => busy($('singing-save'), async () => {
  await api('/api/singing/config', { method: 'POST', body: { songs_dirs: $('songs-dirs').value,
    download_dir: $('download-dir').value.trim(), cache_dir: $('cache-dir').value.trim() } });
  await loadSinging(); message('唱歌路径已保存，立即生效。');
}));
$('song-download').addEventListener('click', () => busy($('song-download'), async () => {
  const result = await api('/api/singing/download', { method: 'POST', body: { url: $('song-url').value.trim(), title: $('song-title').value.trim() } });
  singingDownload = result.download_id; $('song-url').value = ''; $('song-title').value = '';
  message(result.announce ? '已开始下载。练好后鲸鱼娘会在设备上问你要不要听。' : '已开始下载。现在没有设备连着，练好后直接对鲸鱼娘点这首歌就行。');
  await pollSingingDownload();
}));
$('history-device').addEventListener('change',()=>{historyGeneration++;historyLoading=false;historyCursor=historyNext=0;historyControls(false);loadHistory().catch(error=>message(error.message,true));});
$('history-clear').addEventListener('click',()=>clearHistory().catch(error=>message(error.message,true)));
$('history-refresh').addEventListener('click',()=>busy($('history-refresh'),()=>loadHistory()));
$('history-verbose').addEventListener('change',()=>loadHistory(historyCursor).catch(error=>message(error.message,true)));
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
  const response=await fetch('/api/tts-test',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify({tts_provider:$('tts-provider').value,tts_speed:Number($('tts-speed').value||1.0),breeze_voice:$('breeze-voice').value,preview_mode:mode})});
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
