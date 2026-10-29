'use strict';
const $ = id => document.getElementById(id);
let csrf = '', revision = '', pollTimer = null;
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
  $('console').hidden = true; $('login-card').hidden = false; $('runtime').textContent = '未登录';
  clearInterval(pollTimer); pollTimer = null; csrf = ''; $('device-token').value = ''; $('api-key').value = '';
  revision = ''; $('issued').hidden = true; $('expires').textContent = '';
  for (const id of ['base-url', 'model-name', 'prompt', 'tts-voice', 'voice-url', 'device-id', 'client-id']) $(id).value = '';
  $('tools').replaceChildren(); $('devices').replaceChildren(); $('tool-policy').textContent = '';
}
function draft() {
  return { revision, base_url: $('base-url').value.trim(), model_name: $('model-name').value.trim(),
    api_key: $('api-key').value.trim(), clear_api_key: $('clear-key').checked,
    prompt: $('prompt').value, tts_voice: $('tts-voice').value.trim(), tool_calling: $('tool-calling').checked };
}
async function loadSettings() {
  const data = await api('/api/settings'); revision = data.revision;
  for (const [id, value] of [['base-url', data.base_url], ['model-name', data.model_name],
    ['prompt', data.prompt], ['tts-voice', data.tts_voice], ['voice-url', data.websocket]]) $(id).value = value || '';
  $('key-state').textContent = data.api_key_set ? '已保存 · 不回显' : '尚未配置';
  $('api-key').value = ''; $('clear-key').checked = false;
  $('tool-calling').checked = data.tool_calling;
  $('asr-name').textContent = data.asr; $('tts-name').textContent = data.tts;
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
  if (!data.devices.length) $('devices').textContent = '暂无语音连接。可先从 Flipper 设置查看设备身份并签发 Token。';
  for (const item of data.devices) {
    const row = document.createElement('div'); row.className = 'device';
    const id = document.createElement('div'); id.textContent = `Device ID: ${item.device_id}`; row.append(id);
    const client = document.createElement('div'); client.textContent = `Client ID: ${item.client_id}`; row.append(client);
    const tools = document.createElement('div'); tools.textContent = `设备上报工具：${item.tools.join(', ') || '等待 MCP 协商'}`; row.append(tools);
    const use = document.createElement('button'); use.className = 'secondary'; use.type = 'button'; use.textContent = '使用这组身份';
    use.addEventListener('click', () => { $('device-id').value = item.device_id; $('client-id').value = item.client_id; }); row.append(use);
    $('devices').append(row);
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
  document.querySelector('.save-bar').hidden = button.dataset.panel === 'device-panel';
}));
$('save').addEventListener('click', () => busy($('save'), async () => {
  const result = await api('/api/settings', { method: 'POST', body: draft() }); revision = result.revision;
  await loadSettings(); await status(); message('已保存，语音服务正在重新应用设置。');
}));
$('test-model').addEventListener('click', () => busy($('test-model'), async () => {
  const result = await api('/api/model-test', { method: 'POST', body: draft() });
  message(result.tool_call_returned ? '模型接口与工具调用测试通过。' : '接口可达，但未返回工具调用；请确认模型支持 tools。', !result.tool_call_returned);
}));
$('device-form').addEventListener('submit', event => {
  event.preventDefault(); busy(event.submitter, async () => {
    const result = await api('/api/device-token', { method: 'POST', body: { device_id: $('device-id').value.trim(), client_id: $('client-id').value.trim() } });
    $('device-token').value = result.token; $('expires').textContent = `有效期约 ${Math.round(result.expires_in / 86400)} 天，请复制到 Flipper。`;
    $('issued').hidden = false; message('设备凭据已生成，仅在本次页面中显示。');
  });
});
$('copy-token').addEventListener('click', () => busy($('copy-token'), async () => {
  await navigator.clipboard.writeText($('device-token').value); message('已复制设备 Token。');
}));
(async () => { try { const session = await api('/api/session'); csrf = session.csrf; await openConsole(); } catch { showLogin(); } })();
