const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
class Element {
  constructor(){this.value='';this.hidden=false;this.children=[];this.textContent='';this.options=[];this.disabled=false;}
  addEventListener(){}
  pause(){}
  play(){return Promise.resolve();}
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
  let calls=0;
  context.confirm=()=>false;
  context.fetch=async()=>{calls++;throw new Error('cancel must not send');};
  await vm.runInContext('clearHistory()',context);
  assert.equal(calls,0);
  let oldResponse,clearResponse;
  const empty={events:[],devices:[],before:0,write_error:false};
  const response=value=>({ok:true,text:async()=>JSON.stringify(value)});
  context.fetch=()=>new Promise(resolve=>{oldResponse=resolve;});
  const oldPage=vm.runInContext('loadHistory()',context);
  let posted;
  context.confirm=()=>true;
  context.fetch=async(url,options)=>{
    if(url==='/api/history/clear'){
      posted=options;
      return new Promise(resolve=>{clearResponse=resolve;});
    }
    return response(empty);
  };
  vm.runInContext("csrf='test-csrf'",context);
  const clearing=vm.runInContext('clearHistory()',context);
  assert.equal(get('history-device').disabled,true);
  assert.equal(get('history-clear').disabled,true);
  assert.equal(posted.method,'POST');
  assert.equal(posted.headers['X-CSRF-Token'],'test-csrf');
  assert.deepEqual(JSON.parse(posted.body),{scope:'device',device:'device-a'});
  oldResponse(response({events:[{time:0,kind:'user',text:'stale page'}],devices:[],before:99}));
  await oldPage;
  assert.notEqual(get('history-events').children[0]?.children[1]?.textContent,'stale page');
  clearResponse(response({deleted:12,reset_sessions:1,reset_pending:0}));
  await clearing;
  assert.equal(get('history-events').children.length,0);
  assert.equal(get('history-clear').disabled,false);
  assert.equal(vm.runInContext('historyCursor+historyNext',context),0);
  assert.match(get('message').textContent,/12/);
  // With no device selected, the destructive request explicitly selects all.
  get('history-device').value='';
  context.fetch=async(url,options)=>{
    if(url==='/api/history/clear'){
      assert.deepEqual(JSON.parse(options.body),{scope:'all',device:''});
      return response({deleted:0,reset_sessions:0,reset_pending:0});
    }
    return response(empty);
  };
  await vm.runInContext('clearHistory()',context);
  // Failed deletion must leave records visible and restore the controls.
  const stillThere={events:[{time:0,kind:'user',text:'kept after failure'}],devices:[],before:0};
  context.fetch=async(url)=>url==='/api/history/clear'
    ?{ok:false,status:503,text:async()=>JSON.stringify({error:'disk unavailable'})}
    :response(stillThere);
  await vm.runInContext('clearHistory()',context);
  assert.equal(get('history-events').children[0].children[1].textContent,'kept after failure');
  assert.equal(get('history-clear').disabled,false);
  assert.equal(get('message').textContent,'disk unavailable');
  // Preview mode/download labels stay in sync; late preview cannot undo logout.
  let blobs=0,previewBody;
  context.URL={createObjectURL:()=>`blob:test-${++blobs}`,revokeObjectURL(){}};
  context.fetch=async(url,options)=>{
    assert.equal(url,'/api/tts-test');previewBody=JSON.parse(options.body);
    return {ok:true,blob:async()=>({type:'audio/wav'})};
  };
  await vm.runInContext("testTts('opus')",context);
  assert.equal(previewBody.preview_mode,'opus');
  assert.equal(get('download-tts').download,'tts-preview-opus.wav');
  assert.equal(get('download-tts').href,'blob:test-1');
  assert.equal(get('download-tts').hidden,false);
  let oldPreview;
  context.fetch=()=>new Promise(resolve=>{oldPreview=resolve;});
  const previewPending=vm.runInContext("testTts('source')",context);
  vm.runInContext('showLogin()',context);
  oldPreview({ok:true,blob:async()=>({type:'audio/wav'})});await previewPending;
  assert.equal(get('download-tts').hidden,true);
  assert.equal(blobs,1);
  get('console').hidden=false;
  let resolveFetch;
  context.fetch=()=>new Promise(resolve=>{resolveFetch=resolve;});
  const pending=vm.runInContext('loadHistory()',context);
  vm.runInContext('showLogin()',context);
  resolveFetch({ok:true,text:async()=>JSON.stringify({events:[{kind:'user',text:'late'}],devices:[],before:0})});
  await pending;
  assert.equal(get('history-events').children.length,0); // Logout cannot be undone by an old response.
  console.log('PASS: admin history safe rendering, pagination, clear scopes/confirmation/CSRF and stale-response fencing');
})().catch(error=>{console.error(error);process.exitCode=1;});
