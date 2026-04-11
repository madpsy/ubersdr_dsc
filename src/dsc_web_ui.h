/* -*- c++ -*- */
/* dsc_web_ui.h — Embedded HTML/CSS/JS for the DSC decoder web UI.
 * Auto-included by dsc_rx_from_ubersdr.cpp.  Not a general-purpose header.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DSC_WEB_UI_H
#define DSC_WEB_UI_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static std::string json_esc_html(const std::string &s) {
    std::string o; o.reserve(s.size()+8);
    for (char c:s) { switch(c){
        case '"': o+="\\\"";break; case '\\':o+="\\\\";break;
        case '\n':o+="\\n";break; case '\r':o+="\\r";break;
        default: o+=c; } }
    return o;
}

static std::string make_html_page(const std::string &sdr_url,
                                  const std::vector<int64_t> &freqs,
                                  const std::string &base_path)
{
    std::string fjs="[";
    for (size_t i=0;i<freqs.size();i++){
        if(i)fjs+=",";
        char b[32];snprintf(b,sizeof(b),"%lld",(long long)freqs[i]);fjs+=b;
    }
    fjs+="]";

    std::string h;
    /* ---- DOCTYPE + CSS ---- */
    h+="<!DOCTYPE html><html lang=en><head><meta charset=UTF-8>"
       "<meta name=viewport content='width=device-width,initial-scale=1'>"
       "<title>DSC Decoder</title><style>"
       "*{box-sizing:border-box;margin:0;padding:0}"
       "body{font-family:'Segoe UI',system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
       "display:flex;flex-direction:column;height:100vh;overflow:hidden}"
       "header{background:#16213e;border-bottom:1px solid #0f3460;padding:10px 16px;"
       "display:flex;align-items:center;gap:20px;flex-wrap:wrap;flex-shrink:0}"
       "header h1{font-size:1.1rem;color:#e94560;letter-spacing:2px;text-transform:uppercase}"
       ".ii{display:flex;flex-direction:column;gap:2px}"
       ".il{font-size:.62rem;color:#888;text-transform:uppercase;letter-spacing:1px}"
       ".iv{font-size:.9rem;color:#53d8fb}"
       "#sd{width:9px;height:9px;border-radius:50%;background:#555;display:inline-block;margin-right:5px}"
       "#sd.c{background:#4caf50}#sd.d{background:#e94560}"
       ".tb{background:#16213e;border-bottom:1px solid #0f3460;display:flex;flex-shrink:0}"
       ".tbtn{background:0 0;border:none;color:#888;padding:10px 20px;cursor:pointer;"
       "font-size:.85rem;font-family:inherit;border-bottom:2px solid transparent}"
       ".tbtn:hover{color:#53d8fb}.tbtn.a{color:#53d8fb;border-bottom-color:#e94560}"
       ".tc{display:none;flex:1;overflow:hidden;flex-direction:column}.tc.a{display:flex}"
       ".fb{background:#16213e;padding:8px 16px;display:flex;align-items:center;gap:12px;"
       "border-bottom:1px solid #0f3460;flex-shrink:0}"
       ".fb label{font-size:.75rem;color:#888;text-transform:uppercase}"
       ".fb select,.fb input[type=checkbox]{background:#0d0d1a;color:#53d8fb;border:1px solid #0f3460;"
       "border-radius:4px;padding:4px 8px;font-family:inherit;font-size:.8rem}"
       ".mtw{flex:1;overflow-y:auto}"
       "table{width:100%;border-collapse:collapse;font-size:.76rem}"
       "th{background:#0f3460;color:#53d8fb;padding:6px 8px;text-align:left;position:sticky;"
       "top:0;z-index:1;font-size:.68rem;text-transform:uppercase;letter-spacing:.5px}"
       "td{padding:5px 8px;border-bottom:1px solid #1a1a3e;white-space:nowrap}"
       "tr:hover td{background:#1a2a4e}"
       "tr.dist td{background:rgba(233,69,96,.15)}tr.dist:hover td{background:rgba(233,69,96,.25)}"
       "tr.urg td{background:rgba(255,152,0,.12)}tr.urg:hover td{background:rgba(255,152,0,.22)}"
       "tr.saf td{background:rgba(255,235,59,.08)}tr.saf:hover td{background:rgba(255,235,59,.18)}"
       ".xr{display:none}.xr.s{display:table-row}"
       ".xc{padding:10px 16px;background:#0d0d1a;font-size:.73rem;color:#b0b0b0}"
       ".xc pre{white-space:pre-wrap;word-break:break-all;font-family:'Courier New',monospace}"
       ".ftw{flex:1;overflow-y:auto;padding:8px}.ft{width:100%}.ft td{padding:8px 10px}"
       ".dot{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:6px}"
       ".dot.g{background:#4caf50;box-shadow:0 0 6px #4caf50}"
       ".dot.y{background:#ffeb3b;box-shadow:0 0 4px #ffeb3b}"
       ".dot.r{background:#e94560}.dot.x{background:#555}"
       ".ck{cursor:pointer}.ck:hover{text-decoration:underline;color:#53d8fb}"
       ".ap{padding:24px;display:flex;flex-direction:column;align-items:center;gap:16px}"
       ".ap select{background:#0d0d1a;color:#53d8fb;border:1px solid #0f3460;"
       "border-radius:4px;padding:8px 16px;font-family:inherit;font-size:.9rem;min-width:220px}"
       ".abtn{background:#0f3460;border:1px solid #1a6b8a;color:#53d8fb;border-radius:6px;"
       "padding:10px 24px;cursor:pointer;font-family:inherit;font-size:.9rem}"
       ".abtn:hover{background:#1a6b8a}.abtn.on{background:#1a6b1a;border-color:#4caf50;color:#4caf50}"
       ".lm{width:300px;height:20px;background:#0d0d1a;border:1px solid #0f3460;"
       "border-radius:4px;overflow:hidden}"
       ".lf{height:100%;width:0%;border-radius:4px;"
       "background:linear-gradient(90deg,#1a6b1a,#4caf50,#ffeb3b,#e94560);transition:width .15s}"
       "</style></head><body>";

    /* ---- Header ---- */
    h+="<header><h1>&#x1F4E1; DSC Decoder</h1>"
       "<div class=ii><span class=il>SDR Server</span><span class=iv id=su></span></div>"
       "<div class=ii><span class=il>Channels</span><span class=iv id=cc>0</span></div>"
       "<div class=ii><span class=il>Messages</span><span class=iv id=mc>0</span></div>"
       "<div class=ii><span class=il>Connected</span><span class=iv id=nc>0</span></div>"
       "<div class=ii><span class=il>Status</span>"
       "<span class=iv><span id=sd></span><span id=st>Connecting&hellip;</span></span></div>"
       "</header>";

    /* ---- Tab bar ---- */
    h+="<div class=tb>"
       "<button class='tbtn a' data-t=msg>&#x1F4E8; Messages</button>"
       "<button class=tbtn data-t=freq>&#x1F4FB; Frequency Monitor</button>"
       "<button class=tbtn data-t=aud>&#x1F50A; Audio Preview</button></div>";

    /* ---- Tab 1: Messages ---- */
    h+="<div class='tc a' id=t-msg>"
       "<div class=fb><label>Frequency:</label>"
       "<select id=ff><option value=all>All Frequencies</option></select>"
       "<label>Category:</label>"
       "<select id=cf><option value=all>All</option>"
       "<option value=Distress>Distress</option><option value=Urgency>Urgency</option>"
       "<option value=Safety>Safety</option><option value=Routine>Routine</option></select>"
       "<label style='margin-left:auto'>Valid only:</label><input type=checkbox id=vf></div>"
       "<div class=mtw><table><thead><tr>"
       "<th>Time</th><th>Freq</th><th>Format</th><th>Category</th>"
       "<th>Self ID</th><th>Country</th><th>Address</th><th>Addr Country</th>"
       "<th>TC1</th><th>TC2</th><th>Distress</th><th>Position</th>"
       "<th>EOS</th><th>ECC</th></tr></thead>"
       "<tbody id=mb></tbody></table></div></div>";

    /* ---- Tab 2: Frequency Monitor ---- */
    h+="<div class=tc id=t-freq>"
       "<div class=ftw><table class=ft><thead><tr>"
       "<th>Frequency (MHz)</th><th>Band</th><th>Status</th>"
       "<th>Messages</th><th>Valid</th><th>Errors</th><th>Error Rate</th>"
       "<th>Last Message</th><th>Avg RSSI</th><th>Reconnects</th>"
       "</tr></thead><tbody id=fb2></tbody></table></div></div>";

    /* ---- Tab 3: Audio Preview ---- */
    h+="<div class=tc id=t-aud><div class=ap>"
       "<h2 style='color:#53d8fb'>Audio Preview</h2>"
       "<p style='color:#888;font-size:.8rem'>Select a frequency to listen to raw SDR audio</p>"
       "<select id=af></select>"
       "<button class=abtn id=ab>&#x1F50A; Start Audio</button>"
       "<div class=lm><div class=lf id=lf></div></div>"
       "<p style='color:#555;font-size:.7rem' id=as2>Audio stopped</p>"
       "</div></div>";

    /* ---- JavaScript ---- */
    h+="<script>";
    h+="var SU='"+json_esc_html(sdr_url)+"';";
    h+="var FR="+fjs+";";
    h+="var BP='"+json_esc_html(base_path)+"';";

    h+=
    "document.getElementById('su').textContent=SU;"
    "document.getElementById('cc').textContent=FR.length;"
    "var ff=document.getElementById('ff'),af=document.getElementById('af');"
    "FR.forEach(function(f){var m=(f/1e6).toFixed(4);"
    "ff.add(new Option(m+' MHz',f));af.add(new Option(m+' MHz',f))});"
    "document.querySelectorAll('.tbtn').forEach(function(b){b.addEventListener('click',function(){"
    "document.querySelectorAll('.tbtn').forEach(function(x){x.classList.remove('a')});"
    "document.querySelectorAll('.tc').forEach(function(x){x.classList.remove('a')});"
    "b.classList.add('a');document.getElementById('t-'+b.dataset.t).classList.add('a')})});"
    "var sd=document.getElementById('sd'),st=document.getElementById('st');"
    "var mc=document.getElementById('mc'),nc=document.getElementById('nc');"
    "var mb=document.getElementById('mb'),fb2=document.getElementById('fb2');"
    "var cf=document.getElementById('cf'),vf=document.getElementById('vf');"
    "var ab=document.getElementById('ab'),lfEl=document.getElementById('lf');"
    "var as2El=document.getElementById('as2');"
    "var ws=null,totalMsg=0;"
    "var actx=null,aon=false,asr=12000,aq=[],ant=0;"
    "function aflush(){if(!actx||!aq.length)return;var n=actx.currentTime;"
    "if(ant<n)ant=n+.05;while(aq.length){var s=aq.shift();"
    "var b=actx.createBuffer(1,s.length,asr);b.copyToChannel(s,0);"
    "var src=actx.createBufferSource();src.buffer=b;src.connect(actx.destination);"
    "src.start(ant);ant+=b.duration}}"
    "function astart(){if(aon||!ws)return;var f=parseInt(af.value);if(!f)return;"
    "actx=new(window.AudioContext||window.webkitAudioContext)({sampleRate:asr});"
    "ant=0;aq=[];aon=true;ab.classList.add('on');ab.textContent='\\uD83D\\uDD0A Stop Audio';"
    "as2El.textContent='Streaming '+(f/1e6).toFixed(4)+' MHz';"
    "ws.send(JSON.stringify({type:'audio_preview',enable:true,frequency:f}))}"
    "function astop(){if(!aon)return;aon=false;aq=[];"
    "if(actx){actx.close();actx=null}ab.classList.remove('on');"
    "ab.textContent='\\uD83D\\uDD0A Start Audio';as2El.textContent='Audio stopped';"
    "lfEl.style.width='0%';if(ws)ws.send(JSON.stringify({type:'audio_preview',enable:false}))}"
    "ab.addEventListener('click',function(){if(aon)astop();else astart()});"
    "af.addEventListener('change',function(){if(aon){astop();astart()}});"
    "function fmtTime(iso){if(!iso)return'\\u2014';try{return new Date(iso).toLocaleTimeString('en-GB',"
    "{hour:'2-digit',minute:'2-digit',second:'2-digit'})}catch(e){return iso}}"
    "function fmtTs(ep){if(!ep||ep===0)return'Never';var d=new Date(ep*1000),n=new Date();"
    "var s=Math.floor((n-d)/1000);if(s<60)return s+'s ago';if(s<3600)return Math.floor(s/60)+'m ago';"
    "if(s<86400)return Math.floor(s/3600)+'h ago';return d.toLocaleDateString()}"
    "function catClass(c){if(!c)return'';var l=c.toLowerCase();"
    "if(l==='distress')return'dist';if(l==='urgency')return'urg';if(l==='safety')return'saf';return''}"
    "function applyFilters(){var fv=ff.value,cv=cf.value,vo=vf.checked;"
    "mb.querySelectorAll('tr[data-msg]').forEach(function(r){"
    "var mf=r.dataset.freq,mc2=r.dataset.cat,mv=r.dataset.valid;"
    "var show=true;"
    "if(fv!=='all'&&mf!==fv)show=false;"
    "if(cv!=='all'&&mc2!==cv)show=false;"
    "if(vo&&mv!=='true')show=false;"
    "r.style.display=show?'':'none';"
    "var xr=r.nextElementSibling;if(xr&&xr.classList.contains('xr'))xr.style.display=show?'':'none'})}"
    "ff.addEventListener('change',applyFilters);"
    "cf.addEventListener('change',applyFilters);"
    "vf.addEventListener('change',applyFilters);"
    "function addMsg(d){totalMsg++;mc.textContent=totalMsg;"
    "var cat=d.hasCategory?d.category:'';var cls=catClass(cat);"
    "var tr=document.createElement('tr');tr.dataset.msg='1';"
    "tr.dataset.freq=String(d.rxFrequencyHz||d.frequencyHz||'');"
    "tr.dataset.cat=cat;tr.dataset.valid=d.valid&&d.eccOk?'true':'false';"
    "if(cls)tr.className=cls;"
    "var eOk=d.eccOk;var eS=eOk?'OK':'ERR';var eC=eOk?'color:#4caf50':'color:#e94560';"
    "tr.innerHTML='<td>'+fmtTime(d.receivedAt)+'</td>'"
    "+'<td>'+(d.rxFrequencyMHz||'')+'</td>'"
    "+'<td>'+(d.formatSpecifier||'')+'</td>'"
    "+'<td>'+cat+'</td>'"
    "+'<td>'+(d.selfId||'')+'</td>'"
    "+'<td>'+(d.selfCountry||'')+'</td>'"
    "+'<td>'+(d.hasAddress?d.address:'')+'</td>'"
    "+'<td>'+(d.addressCountry||'')+'</td>'"
    "+'<td>'+(d.hasTelecommand1?d.telecommand1:'')+'</td>'"
    "+'<td>'+(d.hasTelecommand2?d.telecommand2:'')+'</td>'"
    "+'<td>'+(d.hasDistressNature?d.distressNature:'')+'</td>'"
    "+'<td>'+(d.hasPosition?d.position:'')+'</td>'"
    "+'<td>'+(d.eos||'')+'</td>'"
    "+'<td style=\"'+eC+'\">'+eS+'</td>';"
    "tr.style.cursor='pointer';"
    "var xr=document.createElement('tr');xr.className='xr';"
    "var xc=document.createElement('td');xc.className='xc';xc.colSpan=14;"
    "xc.innerHTML='<pre>'+JSON.stringify(d,null,2).replace(/</g,'&lt;')+'</pre>';"
    "xr.appendChild(xc);"
    "tr.addEventListener('click',function(){xr.classList.toggle('s')});"
    "mb.prepend(xr);mb.prepend(tr);applyFilters()}"
    "function updateFreqs(data){fb2.innerHTML='';var conn=0;"
    "data.forEach(function(m){"
    "if(m.connected)conn++;"
    "var tr=document.createElement('tr');"
    "var sc=m.connected?(m.messageCount>0?'g':'y'):(m.enabled?'r':'x');"
    "var sl=m.connected?(m.messageCount>0?'Active':'Idle'):(m.enabled?'Disconnected':'Disabled');"
    "tr.innerHTML='<td class=ck>'+m.frequencyMHz+'</td>'"
    "+'<td>'+m.band+'</td>'"
    "+'<td><span class=\"dot '+sc+'\"></span>'+sl+'</td>'"
    "+'<td>'+m.messageCount+'</td>'"
    "+'<td>'+m.validMessageCount+'</td>'"
    "+'<td>'+m.totalErrors+'</td>'"
    "+'<td>'+m.errorRate.toFixed(1)+'%</td>'"
    "+'<td>'+fmtTs(m.lastMessageTime)+'</td>'"
    "+'<td>'+m.avgRssi.toFixed(1)+'</td>'"
    "+'<td>'+m.reconnectCount+'</td>';"
    "tr.querySelector('.ck').addEventListener('click',function(){"
    "ff.value=String(m.frequencyHz);applyFilters();"
    "document.querySelectorAll('.tbtn').forEach(function(x){x.classList.remove('a')});"
    "document.querySelectorAll('.tc').forEach(function(x){x.classList.remove('a')});"
    "document.querySelector('[data-t=msg]').classList.add('a');"
    "document.getElementById('t-msg').classList.add('a')});"
    "fb2.appendChild(tr)});"
    "nc.textContent=conn}"
    "function connect(){"
    "var proto=location.protocol==='https:'?'wss':'ws';"
    "ws=new WebSocket(proto+'://'+location.host+BP+'/ws');"
    "ws.onopen=function(){sd.className='c';st.textContent='Connected';"
    "if(aon){var f=parseInt(af.value);if(f)ws.send(JSON.stringify("
    "{type:'audio_preview',enable:true,frequency:f}))}};"
    "ws.onclose=function(){sd.className='d';st.textContent='Disconnected \\u2014 reconnecting\\u2026';"
    "ws=null;setTimeout(connect,3000)};"
    "ws.onerror=function(){ws.close()};"
    "ws.binaryType='arraybuffer';"
    "ws.onmessage=function(e){"
    "if(e.data instanceof ArrayBuffer){"
    "var v=new DataView(e.data);var t=v.getUint8(0);"
    "if(t===0x04&&aon&&actx){"
    "var n=(e.data.byteLength-9)/2;if(n>0){"
    "var f32=new Float32Array(n);var pk=0;"
    "for(var i=0;i<n;i++){var sv=v.getInt16(9+i*2,true)/32768.0;f32[i]=sv;if(Math.abs(sv)>pk)pk=Math.abs(sv)}"
    "aq.push(f32);aflush();"
    "lfEl.style.width=Math.min(100,pk*200)+'%'}}"
    "return}"
    "var txt=typeof e.data==='string'?e.data:null;"
    "if(!txt)return;"
    "try{var msg=JSON.parse(txt);"
    "if(msg.type==='message'&&msg.data)addMsg(msg.data);"
    "if(msg.type==='history'&&Array.isArray(msg.data)){"
    /* History arrives oldest-first; addMsg() prepends each row, so iterate
     * in reverse so the oldest ends up at the bottom of the table. */
    "for(var hi=msg.data.length-1;hi>=0;hi--)addMsg(msg.data[hi]);}"
    "if(msg.type==='metrics'&&msg.data)updateFreqs(msg.data);"
    "}catch(ex){}}};"
    "connect();"
    "</script></body></html>";

    return h;
}

#endif /* DSC_WEB_UI_H */
