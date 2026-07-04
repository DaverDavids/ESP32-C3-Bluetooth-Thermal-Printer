const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en"><head><title>C3 Printer</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;padding:10px;max-width:640px;margin:0 auto}
h1{font-size:18px;color:#a78bfa;padding:10px 0 6px;text-align:center;letter-spacing:1px}
.card{background:#16213e;border:1px solid #2d2d5a;border-radius:8px;padding:10px;margin-bottom:10px}
h2{font-size:13px;color:#7c86c9;border-bottom:1px solid #2d2d5a;padding-bottom:5px;margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}
.stat{padding:3px 10px;border-radius:12px;font-size:11px;font-weight:bold;display:inline-block;margin-right:6px}
.ok{background:#1a3a2a;color:#4ade80;border:1px solid #166534}
.err{background:#3a1a1a;color:#f87171;border:1px solid #7f1d1d}
input[type=text],input[type=number],select,textarea{
  background:#0f0f23;color:#e0e0e0;border:1px solid #3d3d6b;border-radius:4px;padding:4px 6px;font-size:12px;width:100%}
input[type=checkbox]{width:16px;height:16px;cursor:pointer;accent-color:#a78bfa}
.line-row{display:flex;gap:4px;align-items:center;margin-bottom:5px;background:#0f0f23;padding:5px;border-radius:5px;border:1px solid #2d2d5a}
.line-row input[type=text]{flex:1;min-width:0}
.ctl{display:flex;flex-direction:column;align-items:center;gap:2px;flex-shrink:0}
.ctl.size-ctl{width:80px}
.ctl.align-ctl{width:60px}
.ctl.check-ctl{width:32px}
.tiny-lbl{font-size:9px;color:#7c86c9;white-space:nowrap;text-align:center}
.line-num{font-size:10px;color:#555;width:12px;text-align:center;flex-shrink:0}
button{padding:8px 14px;border:none;border-radius:5px;cursor:pointer;font-size:13px;font-weight:bold;color:#fff;width:100%;margin-top:6px}
button.save{background:#6d28d9}
button.test{background:#0e7490;width:auto;padding:4px 10px;font-size:11px;margin-top:0;float:right}
button.connect{background:#065f46}
button.feed-btn{background:#374151}
button.print-btn{background:#1d4ed8}
.feed-row{display:flex;gap:6px;margin-top:5px}
.feed-row button{flex:1}
.section-footer{display:flex;align-items:center;justify-content:space-between;margin-top:6px;font-size:12px;color:#9ca3af}
.section-footer input[type=number]{width:44px;text-align:center}
label.en-lbl{display:flex;align-items:center;gap:4px;font-size:12px;cursor:pointer;color:#a78bfa;float:right}
.pts-filter{margin-top:8px;font-size:12px;color:#9ca3af}
textarea{height:56px;resize:vertical;font-family:monospace}
</style></head>
<body>
<h1>&#127381; C3 Printer</h1>

<div class="card">
  <h2>Status</h2>
  <div id="ps" class="stat err">Printer: --</div>
  <div id="ts" class="stat err">Twitch: --</div>
  <button class="connect" onclick="doConnect()" style="margin-top:8px">Connect Printer</button>
</div>

<div id="cfg"></div>

<div class="card">
  <button class="save" onclick="save()">&#128190; Save All Configuration</button>
</div>

<div class="card">
  <h2>Manual Test Print</h2>
  <textarea id="t_txt">Hello! Caf&#233; resum&#233; na&#239;ve &#1055;&#1088;&#1080;&#1074;&#1077;&#1090; &#12484; ¯\_(ツ)_/¯ 🤷</textarea>
  <div class="line-row" style="margin-top:6px">
    <div class="ctl size-ctl">
      <select id="t_s"></select>
      <span class="tiny-lbl">Size</span>
    </div>
    <div class="ctl align-ctl">
      <select id="t_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select>
      <span class="tiny-lbl">Align</span>
    </div>
    <div class="ctl check-ctl"><input type="checkbox" id="t_b" checked><span class="tiny-lbl">Bold</span></div>
    <div class="ctl check-ctl"><input type="checkbox" id="t_i"><span class="tiny-lbl">Invert</span></div>
  </div>
  <div class="feed-row">
    <button class="print-btn" onclick="testPrint()">&#128424; Print</button>
    <button class="feed-btn" onclick="feed()">&#128196; Feed 3</button>
  </div>
</div>

<div class="card">
  <h2>Upload Font Files</h2>
  <form id="fontForm">
    <input type="file" id="fontFiles" multiple accept=".vlw">
    <button type="button" onclick="uploadFonts()" class="save">Upload to LittleFS</button>
    <div id="uploadStatus" style="font-size:11px;margin-top:6px"></div>
  </form>
  <div style="margin-top:8px;font-size:12px;color:#9ca3af">
    <a href="/fsinfo" target="_blank" style="color:#a78bfa">Filesystem info</a>
  </div>
</div>

<script>
// Size values are PRINT_SCALE multipliers: 1=Small(16px), 2=Medium(32px), 3=Large(48px)
const SIZES = [
  [1,"Small"],[2,"Medium"],[3,"Large"]
];

const evts   = ['sub','bit','pts','raid'];
const titles = ['Subscriptions','Bits','Points','Raids'];

function sizeOpts(sel) {
  return SIZES.map(([id,lbl])=>`<option value="${id}"${id==sel?' selected':''}>${lbl}</option>`).join('');
}
function alignOpts(sel) {
  return ['Left','Center','Right'].map((t,i)=>
    `<option value="${i}"${i==sel?' selected':''}>${t}</option>`).join('');
}

function render() {
  let h = '';
  evts.forEach((k,i) => {
    h += `<div class="card">
      <h2>${titles[i]}
        <label class="en-lbl"><input type="checkbox" id="${k}_e"> Enabled</label>
      </h2>`;
    for(let l=0;l<3;l++) {
      h += `<div class="line-row">
        <span class="line-num">${l+1}</span>
        <input type="text" id="${k}${l}_m" placeholder="Line ${l+1} ({user} {amount} {reward})">
        <div class="ctl size-ctl">
          <select id="${k}${l}_s">${sizeOpts(2)}</select>
          <span class="tiny-lbl">Size</span>
        </div>
        <div class="ctl align-ctl">
          <select id="${k}${l}_a">${alignOpts(1)}</select>
          <span class="tiny-lbl">Align</span>
        </div>
        <div class="ctl check-ctl"><input type="checkbox" id="${k}${l}_b"><span class="tiny-lbl">Bold</span></div>
        <div class="ctl check-ctl"><input type="checkbox" id="${k}${l}_i"><span class="tiny-lbl">Inv</span></div>
      </div>`;
    }
    h += `<div class="section-footer">
      <span>Feed lines: <input type="number" id="${k}_f" value="3" min="0" max="20"></span>
      <button class="test" onclick="testEvt('${k}')">&#129514; Test ${titles[i]}</button>
    </div>`;
    if(k==='pts') {
      h += `<div class="pts-filter"><label>Custom Reward ID filter (blank = all):<br>
        <input type="text" id="pts_filter" placeholder="a1b2c3d4-e5f6-..."></label></div>`;
    }
    h += `</div>`;
  });
  document.getElementById('cfg').innerHTML = h;
  document.getElementById('t_s').innerHTML = sizeOpts(2);
}

function load() {
  render();
  fetch('/gcfg').then(r=>r.json()).then(d=>{
    evts.forEach(k=>{
      let el;
      el=document.getElementById(`${k}_e`); if(el) el.checked=d[`${k}_e`];
      el=document.getElementById(`${k}_f`); if(el) el.value=d[`${k}_f`];
      for(let l=0;l<3;l++){
        el=document.getElementById(`${k}${l}_m`); if(el) el.value=d[`${k}${l}_m`]||'';
        el=document.getElementById(`${k}${l}_s`); if(el) el.value=d[`${k}${l}_s`];
        el=document.getElementById(`${k}${l}_a`); if(el) el.value=d[`${k}${l}_a`];
        el=document.getElementById(`${k}${l}_b`); if(el) el.checked=d[`${k}${l}_b`];
        el=document.getElementById(`${k}${l}_i`); if(el) el.checked=d[`${k}${l}_i`];
      }
    });
    el=document.getElementById('pts_filter'); if(el) el.value=d.pts_filter||'';
  }).catch(()=>{});
}

function save() {
  let p = new URLSearchParams();
  evts.forEach(k=>{
    p.append(`${k}_e`,document.getElementById(`${k}_e`).checked?1:0);
    p.append(`${k}_f`,document.getElementById(`${k}_f`).value);
    for(let l=0;l<3;l++){
      p.append(`${k}${l}_m`,document.getElementById(`${k}${l}_m`).value);
      p.append(`${k}${l}_s`,document.getElementById(`${k}${l}_s`).value);
      p.append(`${k}${l}_a`,document.getElementById(`${k}${l}_a`).value);
      p.append(`${k}${l}_b`,document.getElementById(`${k}${l}_b`).checked?1:0);
      p.append(`${k}${l}_i`,document.getElementById(`${k}${l}_i`).checked?1:0);
    }
  });
  p.append('pts_filter',document.getElementById('pts_filter').value.trim());
  fetch('/tcfg',{method:'POST',body:p}).then(r=>r.text()).then(alert).catch(e=>alert(e));
}

function testEvt(k) {
  let p = new URLSearchParams();
  p.append('type',k);
  p.append('f',document.getElementById(`${k}_f`).value);
  for(let l=0;l<3;l++){
    p.append(`m${l}`,document.getElementById(`${k}${l}_m`).value);
    p.append(`s${l}`,document.getElementById(`${k}${l}_s`).value);
    p.append(`a${l}`,document.getElementById(`${k}${l}_a`).value);
    p.append(`b${l}`,document.getElementById(`${k}${l}_b`).checked?1:0);
    p.append(`i${l}`,document.getElementById(`${k}${l}_i`).checked?1:0);
  }
  fetch('/test_evt',{method:'POST',body:p}).then(r=>r.text()).then(alert).catch(e=>alert(e));
}

function testPrint() {
  let p = new URLSearchParams();
  p.append('txt',document.getElementById('t_txt').value);
  p.append('sz', document.getElementById('t_s').value);
  p.append('al', document.getElementById('t_al').value);
  p.append('b',  document.getElementById('t_b').checked?1:0);
  p.append('inv',document.getElementById('t_i').checked?1:0);
  fetch('/p',{method:'POST',body:p}).then(r=>r.text()).then(alert).catch(e=>alert(e));
}

function feed()      { fetch('/f?lines=3'); }
function doConnect() { fetch('/c').then(r=>r.text()).then(alert); }

async function uploadFonts() {
  const files = document.getElementById('fontFiles').files;
  const status = document.getElementById('uploadStatus');
  status.innerHTML = '';
  for (const file of files) {
    status.innerHTML += `Uploading ${file.name} (${file.size} bytes)...<br>`;
    const fd = new FormData();
    fd.append('file', file);
    try {
      const res = await fetch('/upload', { method: 'POST', body: fd });
      const text = await res.text();
      status.innerHTML += `${file.name}: ${text}<br>`;
    } catch (e) {
      status.innerHTML += `${file.name}: FAILED — ${e}<br>`;
    }
  }
}

setInterval(()=>{
  fetch('/s').then(r=>r.json()).then(d=>{
    let ps=document.getElementById('ps'),ts=document.getElementById('ts');
    ps.className='stat '+(d.printer?'ok':'err');
    ps.textContent='Printer: '+(d.printer?'Connected':'Offline');
    ts.className='stat '+(d.twitch?'ok':'err');
    ts.textContent='Twitch: '+(d.twitch?'Connected':'Offline');
  }).catch(()=>{});
},2000);

load();
</script></body></html>
)rawliteral";
