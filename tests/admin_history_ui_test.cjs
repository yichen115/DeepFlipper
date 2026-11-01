const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
class Element {
  constructor(){this.value='';this.hidden=false;this.children=[];this.textContent='';this.options=[];this.disabled=false;}
  addEventListener(){}
  pause(){}
  removeAttribute(){}
  replaceChildren(){this.children=[];this.textContent='';}
  append(...nodes){this.children.push(...nodes);this.options.push(...nodes);}
}
const elements=new Map();
const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
const context=vm.createContext({document:{getElementById:get,querySelectorAll:()=>[],createElement:()=>new Element()},
  setInterval:()=>0,clearInterval(){},setTimeout:()=>0,clearTimeout(){},console,
  fetch:async()=>({ok:false,status:401,text:async()=>'login required'})});
vm.runInContext(fs.readFileSync('server/main/xiaozhi-server/admin/app.js','utf8'),context);
(async()=>{
  await new Promise(resolve=>setImmediate(resolve)); // Initial anonymous session.
  get('console').hidden=false;
  let requested='';
  context.fetch=async url=>{requested=url;return {ok:true,text:async()=>JSON.stringify({
    events:[{id:8,time:0,device:'device-a',session:'s',kind:'assistant',text:'<img src=x onerror=alert(1)>'}],
    devices:['device-a'],before:8,write_error:false})};};
  await vm.runInContext('loadHistory()',context);
  assert.match(requested,/before=0$/);
  const row=get('history-events').children[0];
  assert.equal(row.children[1].textContent,'<img src=x onerror=alert(1)>');
  assert.equal(row.children[1].children.length,0); // Untrusted transcript never becomes HTML.
  assert.equal(get('history-older').disabled,false);
  get('history-device').value='device-a';
  await vm.runInContext('loadHistory(historyNext)',context);
  assert.match(requested,/device=device-a&before=8$/);
  let resolveFetch;
  context.fetch=()=>new Promise(resolve=>{resolveFetch=resolve;});
  const pending=vm.runInContext('loadHistory()',context);
  vm.runInContext('showLogin()',context);
  resolveFetch({ok:true,text:async()=>JSON.stringify({events:[{kind:'user',text:'late'}],devices:[],before:0})});
  await pending;
  assert.equal(get('history-events').children.length,0); // Logout cannot be undone by an old response.
  console.log('PASS: admin history renders text safely, pages by device and discards logged-out responses');
})().catch(error=>{console.error(error);process.exitCode=1;});
