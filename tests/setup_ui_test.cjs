const assert = require('node:assert/strict');
const {DraftBook,wifiOutcome,actionState,nextStep} = require('../firmware/esp32/main/boards/deepflipper-s3/web/deepflipper_setup.js');
const book = new DraftBook();
assert.deepEqual(book.merge({revision:1}),['wifi','server','audio']);
book.touch('server','token');
book.touch('audio','volume');
const audio=book.ticket('audio');
const applied=book.merge({revision:2},{ack:audio,own:{before:1,after:2}});
assert(!applied.includes('server')); // Saving volume must preserve the pasted token.
assert.equal(book.ticket('server').revision,2); // Our unrelated save may safely advance its base.
assert.deepEqual(book.ticket('server').fields,['token']);
const server=book.ticket('server');
book.touch('server','url'); // Typing continues while the save is in flight.
assert(!book.merge({revision:3},{ack:server,own:{before:2,after:3}}).includes('server'));
assert.deepEqual(book.ticket('server').fields,['token','url']);
book.merge({revision:4}); // Another client changed the profile.
assert.equal(book.ticket('server').revision,3); // No silent overwrite of an unknown revision.
assert.equal(book.ticket('audio').revision,4);
book.merge({revision:4},{rebase:true}); // Explicit refresh keeps input and acknowledges the version.
assert.equal(book.ticket('server').revision,4);
assert.deepEqual(book.ticket('server').fields,['token','url']);
assert.equal(wifiOutcome({state:'failed',request_id:'new-trial'}, {request_id:'new-trial'}),'failed');
assert.equal(wifiOutcome({state:'saved',request_id:'old-trial'}, {request_id:'new-trial'}),null);
assert.equal(wifiOutcome({state:'saved',request_id:'new-trial'}, {request_id:'new-trial'}),'saved');
const ready={wifi:'connected',flipper_connected:true,config_ready:true,microphone:true,volume:20,mute:false,app_state:'idle'};
assert.deepEqual(actionState(ready),{talk:'',speaker:''});
assert.equal(actionState({...ready,wifi:'setup',app_state:'starting'}).speaker,'');
assert.equal(actionState({...ready,wifi:'setup'}).talk,'wifi_offline');
assert.equal(actionState({...ready,config_ready:false}).talk,'needs_config');
assert.equal(actionState({...ready,microphone:false}).talk,'microphone_disabled');
assert.equal(actionState({...ready,mute:true}).speaker,'speaker_muted');
assert.equal(actionState({...ready,volume:0}).speaker,'volume_zero');
assert.equal(actionState({...ready,applying:true}).talk,'busy');
assert.equal(actionState({...ready,app_state:'listening'}).speaker,'busy');
assert.equal(nextStep({...ready,config_ready:false}),'导入或保存服务器连接配置');
assert.equal(nextStep({...ready,server_state:'auth_failed'}),'在服务端重新签发并导入 Token');
assert.equal(nextStep({...ready,wifi_pending:true}),'等待新 Wi-Fi 验证，原配置保留');
console.log('PASS: browser setup workflow preserves drafts, tracks request outcomes and gates audio actions');
