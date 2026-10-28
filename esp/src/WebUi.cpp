#include "WebUi.h"
#if defined(ESP8266)
#include <ESP8266WebServer.h>
typedef ESP8266WebServer WebServerT;
#else
#include <WebServer.h>
typedef WebServer WebServerT;
#endif

static WebServerT server(80);
static CncModule *s_cnc;
static Grbl *s_grbl;
static LinkMaster *s_link;
static File s_upload;
static String s_uploadMsg;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>LinkBus CNC</title>
<style>
:root{--bg:#14161a;--fg:#e8e8e8;--mut:#8a8f98;--card:#1d2026;--acc:#7cc4ff;--bad:#ff7b7b;--ok:#8fe388}
@media (prefers-color-scheme:light){:root{--bg:#f4f5f7;--fg:#16181c;--mut:#5c6370;--card:#fff;--acc:#1769c2;--bad:#c62828;--ok:#2e7d32}}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:640px;margin:auto;padding:16px}h1{font-size:20px;margin:0 0 12px}
.card{background:var(--card);border-radius:10px;padding:14px;margin-bottom:12px}
.dro{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;font:600 20px ui-monospace,monospace}
.dro small{display:block;font:12px system-ui;color:var(--mut)}
table{width:100%;border-collapse:collapse}td{padding:6px 4px;border-top:1px solid #8883}
button{font:inherit;padding:6px 12px;border-radius:7px;border:1px solid #8886;background:transparent;color:var(--fg);cursor:pointer}
button.stop{border-color:var(--bad);color:var(--bad);font-weight:600}.mut{color:var(--mut)}
#st{font-weight:700}
</style></head><body><main>
<h1>LinkBus CNC pendant</h1>
<div class="card"><div>State: <span id="st">…</span> <span id="job" class="mut"></span></div>
<div class="dro" style="margin-top:10px"><div><small>X</small><span id="x">-</span></div><div><small>Y</small><span id="y">-</span></div><div><small>Z</small><span id="z">-</span></div></div>
<p class="mut" id="msg"></p><button class="stop" onclick="post('/api/stop')">STOP</button></div>
<div class="card"><b>G-code files</b><table id="files"></table>
<form id="up" style="margin-top:10px"><input type="file" name="f" accept=".nc,.gcode,.ngc,.tap,.txt,.gba"> <button>Upload</button></form>
<p class="mut">A <code>.gba</code> upload replaces the pendant ROM; it is sent to the GBA next time it boots.</p></div>
<div class="card mut" id="link"></div>
</main><script>
const $=id=>document.getElementById(id);
async function post(u){await fetch(u,{method:'POST'});tick()}
async function tick(){try{const s=await (await fetch('/api/status')).json();
$('st').textContent=s.state;$('job').textContent=s.job?`${s.job} ${(s.progress/10).toFixed(1)}%`:'';
['x','y','z'].forEach((k,i)=>$(k).textContent=(s.wpos[i]/1000).toFixed(3));$('msg').textContent=s.msg;
$('link').textContent=`GBA link: ${s.link} · multiboot: ${s.boot} · retries ${s.retries} · crc errors ${s.crc}`;}catch(e){}}
async function files(){const f=await (await fetch('/api/files')).json();
$('files').innerHTML=f.map(x=>`<tr><td>${x.name}</td><td class="mut">${(x.size/1024).toFixed(1)} KB</td>
<td style="text-align:right"><button onclick="post('/api/run?name='+encodeURIComponent('${x.name}'))">Run</button>
<button onclick="if(confirm('Delete ${x.name}?'))post('/api/delete?name='+encodeURIComponent('${x.name}')).then(files)">✕</button></td></tr>`).join('')||'<tr><td class="mut">No files yet</td></tr>'}
$('up').onsubmit=async e=>{e.preventDefault();const d=new FormData($('up'));const r=await fetch('/upload',{method:'POST',body:d});alert(await r.text());files()};
setInterval(tick,500);tick();files();
</script></body></html>)HTML";

static bool safeName(const String &n)
{
    return n.length() && n.length() <= 40 && n.indexOf('/') < 0 && n.indexOf("..") < 0;
}

void webUiBegin(CncModule &cnc, Grbl &grbl, LinkMaster &link)
{
    s_cnc = &cnc; s_grbl = &grbl; s_link = &link;

    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });

    server.on("/api/status", HTTP_GET, [] {
        const Grbl::Status &st = s_grbl->status();
        static const char *ls[] = { "probing", "booting", "online" };
        String j = "{\"state\":\"";
        j += Grbl::stateName(st.state);
        j += "\",\"wpos\":[" + String(st.wpos[0]) + "," + String(st.wpos[1]) + "," + String(st.wpos[2]) + "]";
        j += ",\"job\":\"" + (s_cnc->jobActive() ? s_cnc->jobName() : String()) + "\"";
        j += ",\"progress\":" + String(s_cnc->progress());
        String msg = s_cnc->lastMessage();
        msg.replace("\\", "\\\\"); msg.replace("\"", "\\\"");
        j += ",\"msg\":\"" + msg + "\"";
        j += ",\"link\":\"" + String(ls[s_link->state()]) + "\"";
        j += ",\"boot\":\"" + String(s_link->lastBootResult()) + "\"";
        j += ",\"retries\":" + String(s_link->retries()) + ",\"crc\":" + String(s_link->crcErrors()) + "}";
        server.send(200, "application/json", j);
    });

    server.on("/api/files", HTTP_GET, [] {
        String j = "[";
#if defined(ESP8266)
        Dir d = LittleFS.openDir(GCODE_DIR);
        bool first = true;
        while (d.next()) {
            if (!d.isFile()) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"name\":\"" + d.fileName() + "\",\"size\":" + String(d.fileSize()) + "}";
        }
#endif
        j += "]";
        server.send(200, "application/json", j);
    });

    server.on("/api/run", HTTP_POST, [] {
        String n = server.arg("name");
        bool ok = safeName(n) && s_cnc->startJob(String(GCODE_DIR "/") + n);
        server.send(ok ? 200 : 409, "text/plain", ok ? "started" : s_cnc->lastMessage());
    });

    server.on("/api/stop", HTTP_POST, [] { s_cnc->stop(); server.send(200, "text/plain", "stopping"); });

    server.on("/api/delete", HTTP_POST, [] {
        String n = server.arg("name");
        bool ok = safeName(n) && !(s_cnc->jobActive() && s_cnc->jobName() == n) &&
                  LittleFS.remove(String(GCODE_DIR "/") + n);
        server.send(ok ? 200 : 400, "text/plain", ok ? "deleted" : "refused");
    });

    server.on("/upload", HTTP_POST,
        [] { server.send(200, "text/plain", s_uploadMsg); },
        [] {
            HTTPUpload &u = server.upload();
            if (u.status == UPLOAD_FILE_START) {
                String n = u.filename;
                n = n.substring(n.lastIndexOf('/') + 1);
                n = n.substring(n.lastIndexOf('\\') + 1);
                String path;
                if (n.endsWith(".gba")) path = ROM_PATH;
                else if (safeName(n)) path = String(GCODE_DIR "/") + n;
                if (!path.length() || (s_cnc->jobActive() && s_cnc->jobName() == n)) {
                    s_uploadMsg = "rejected: " + n;
                    return;
                }
                LittleFS.mkdir("/rom");
                s_upload = LittleFS.open(path, "w");
                s_uploadMsg = s_upload ? "saved " + path : "can't write " + path;
            } else if (u.status == UPLOAD_FILE_WRITE) {
                if (s_upload) s_upload.write(u.buf, u.currentSize);
            } else if (u.status == UPLOAD_FILE_END) {
                if (s_upload) { s_upload.close(); s_uploadMsg += " (" + String(u.totalSize) + " B)"; }
            } else if (u.status == UPLOAD_FILE_ABORTED) {
                if (s_upload) s_upload.close();
                s_uploadMsg = "upload aborted";
            }
        });

    server.begin();
}

void webUiLoop() { server.handleClient(); }
