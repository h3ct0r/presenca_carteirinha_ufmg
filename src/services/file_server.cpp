#include "services/file_server.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WebServer.h>

#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage/sd_tree.h"

static const char* TAG = "fileserver";

// Big enough for WebServer's request parsing plus our handlers (ArduinoJson
// keeps its document on the heap, so this is mostly call depth + Strings).
static constexpr uint32_t TASK_STACK = 8192;
// Same priority as the other service tasks (photo writer, roster, config).
static constexpr UBaseType_t TASK_PRIO = 2;
// How long end() waits for an in-flight request to finish. A stalled client or
// a multi-megabyte transfer can outlast it; the task then winds down on its own.
static constexpr uint32_t TEARDOWN_WAIT_MS = 2000;

static WebServer s_server(80);
static bool s_running = false;
static volatile bool s_stop = false;          // stop request, read by the task
static volatile bool s_task_alive = false;    // cleared by the task as it exits
static SemaphoreHandle_t s_exited = nullptr;  // given by the task once it is done
static bool s_routes_registered = false;      // on() appends, so register once
static File s_upload;
static bool s_upload_ok = false;  // false makes /api/upload answer 500

// Bumped on every successful change to the card. The UI compares it against its
// own last-seen value to know when its cached view of the card went stale (see
// ui/sd_resync.h). Written by this task, read on the LVGL thread — an aligned
// 32-bit word, and the reader only ever tests it for inequality.
static volatile uint32_t s_writes = 0;

// The single-page file manager. The AP has no internet, so a Tailwind-style
// utility CSS is embedded rather than pulled from a CDN. Kept in flash (PROGMEM).
static const char INDEX_HTML[] PROGMEM = R"HTMLDOC(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SD File Manager</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:ui-sans-serif,system-ui,Arial,sans-serif;background:#0f172a;color:#e2e8f0}
.mx-auto{margin-left:auto;margin-right:auto}.max-w-3xl{max-width:48rem}
.p-4{padding:1rem}.p-3{padding:.75rem}.mb-4{margin-bottom:1rem}.mb-2{margin-bottom:.5rem}
.flex{display:flex}.items-center{align-items:center}.justify-between{justify-content:space-between}
.gap-2{gap:.5rem}.wrap{flex-wrap:wrap}.rounded-lg{border-radius:.5rem}.shadow{box-shadow:0 1px 3px rgba(0,0,0,.4)}
.bg-slate-800{background:#1e293b}.text-xl{font-size:1.25rem}.text-sm{font-size:.875rem}
.font-bold{font-weight:700}.font-mono{font-family:ui-monospace,Menlo,Consolas,monospace}
.text-slate-400{color:#94a3b8}.truncate{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:16rem}
.btn{border:0;border-radius:.375rem;padding:.4rem .7rem;font-size:.85rem;cursor:pointer;color:#fff;display:inline-block}
.btn-blue{background:#2563eb}.btn-green{background:#16a34a}.btn-red{background:#dc2626}.btn-slate{background:#475569}
table{width:100%;border-collapse:collapse}
td,th{padding:.5rem;border-bottom:1px solid #1e293b;text-align:left;font-size:.9rem;vertical-align:middle}
th{color:#94a3b8;font-weight:600}
.name{cursor:pointer}.name:hover{color:#60a5fa}
.modal{position:fixed;inset:0;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;padding:1rem}
.hidden{display:none!important}
/* The drop overlay must not swallow the drag events it is reacting to. */
#drop{pointer-events:none;background:rgba(15,23,42,.85)}
#drop .card{max-width:26rem;text-align:center;border:2px dashed #2563eb}
#dropmsg{white-space:pre-wrap;word-break:break-all}
.card{background:#1e293b;border-radius:.5rem;padding:1rem;width:100%;max-width:48rem}
textarea{width:100%;height:60vh;background:#0f172a;color:#e2e8f0;border:1px solid #334155;border-radius:.375rem;padding:.6rem;font-family:ui-monospace,monospace;font-size:.85rem}
a.btn{text-decoration:none}
</style></head>
<body>
<div class="max-w-3xl mx-auto p-4">
  <div class="flex items-center justify-between mb-4">
    <div class="text-xl font-bold">SD File Manager</div>
    <div class="flex gap-2">
      <button class="btn btn-slate" onclick="up()">Up</button>
      <button class="btn btn-blue" onclick="document.getElementById('fileinput').click()">Upload</button>
      <button class="btn btn-slate" onclick="load()">Refresh</button>
      <label class="text-sm text-slate-400" style="display:flex;align-items:center;gap:.3rem;cursor:pointer"><input type="checkbox" onchange="toggleHidden(this.checked)"> Hidden</label>
      <input id="fileinput" type="file" class="hidden" multiple onchange="upload(this.files)">
    </div>
  </div>
  <div id="crumb" class="text-slate-400 font-mono text-sm mb-2">/</div>
  <div class="bg-slate-800 rounded-lg shadow p-3">
    <table><thead><tr><th>Name</th><th>Size</th><th>Actions</th></tr></thead>
    <tbody id="rows"></tbody></table>
  </div>
</div>
<div id="drop" class="modal hidden">
  <div class="card"><div id="dropmsg" class="font-mono text-sm"></div></div>
</div>
<div id="editor" class="modal hidden">
  <div class="card">
    <div class="flex items-center justify-between mb-2">
      <div id="editname" class="font-mono text-sm truncate"></div>
      <div class="flex gap-2">
        <button class="btn btn-green" onclick="save()">Save</button>
        <button class="btn btn-slate" onclick="closeEd()">Close</button>
      </div>
    </div>
    <textarea id="editarea" spellcheck="false"></textarea>
  </div>
</div>
<script>
let cur='/', editing='', showHidden=false, edPushed=false;
const FOLDER='<svg viewBox="0 0 20 20" width="15" height="15" fill="#eab308" style="vertical-align:-2px"><path d="M2 6a2 2 0 0 1 2-2h3l2 2h7a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6z"/></svg>';
const $=id=>document.getElementById(id);
const esc=s=>encodeURIComponent(s);
const q=s=>String(s).replace(/'/g,"\\'");
const join=(d,n)=>d.endsWith('/')?d+n:d+'/'+n;
const fmt=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';
// Only these open in the editor. An allowlist, not a blocklist: a card holds
// .jpg/.tar/.espdl, and a textarea round-trip would corrupt any of them, so an
// unknown (or missing) extension gets Download only.
const TEXT_EXT=['json','jsonl','csv','tsv','txt','md','log','ini','cfg','conf','xml','yml','yaml','html','htm','js','css','sh'];
const isText=n=>{const i=n.lastIndexOf('.');return i>0&&TEXT_EXT.includes(n.slice(i+1).toLowerCase());};
async function load(){
  const d=await (await fetch('/api/list?path='+esc(cur))).json();
  cur=d.path;                       // the server normalizes; keep the URL honest
  history.replaceState({p:cur},'','#'+encodeURI(cur));
  crumb();
  const rows=$('rows'); rows.innerHTML='';
  let items=(d.entries||[]);
  if(!showHidden) items=items.filter(e=>!e.name.startsWith('.'));  // dotfiles hidden by default
  items.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).forEach(e=>{
    const full=join(cur,e.name);
    const nm=e.dir?`<span class="name" onclick="cd('${q(full)}')">${FOLDER} ${e.name}</span>`:e.name;
    const editBtn=isText(e.name)?`<button class="btn btn-slate" onclick="edit('${q(full)}')">Edit</button>`:'';
    const fileAct=e.dir?'':`${editBtn}
      <a class="btn btn-blue" href="/api/download?path=${esc(full)}">Download</a>`;
    const act=`${fileAct}
      <button class="btn btn-slate" onclick="ren('${q(full)}')">Rename</button>
      <button class="btn btn-red" onclick="del('${q(full)}',${e.dir?1:0})">Delete</button>`;
    const tr=document.createElement('tr');
    tr.innerHTML=`<td class="truncate">${nm}</td><td class="text-slate-400">${e.dir?'':fmt(e.size)}</td><td class="flex gap-2 wrap">${act}</td>`;
    rows.appendChild(tr);
  });
}
// Built as DOM nodes, not a template literal: a folder name with a quote or a
// '<' in it would break markup assembled by string (see q(), used by the rows).
function crumb(){
  const c=$('crumb'); c.innerHTML='';
  const parts=cur.split('/').filter(Boolean);
  const seg=(label,path,last)=>{
    const s=document.createElement('span');
    s.textContent=label;
    if(last) s.style.color='#e2e8f0';        // the current folder: shown, not a link
    else{s.className='name';s.addEventListener('click',()=>go(path));}
    c.appendChild(s);
  };
  seg('/','/',parts.length===0);
  parts.forEach((p,i)=>{
    if(i) c.appendChild(document.createTextNode('/'));
    seg(p,'/'+parts.slice(0,i+1).join('/'),i===parts.length-1);
  });
}
// The path lives in the hash, never in the URL path: a reload of /classes/ABC
// would hit the server's 404 handler.
const hashPath=()=>{const h=decodeURIComponent(location.hash.slice(1));return h.startsWith('/')?h:'/';};
function go(p){history.pushState({p},'','#'+encodeURI(p));cur=p;load();}
function cd(p){go(p);}
function toggleHidden(v){showHidden=v;load();}   // a filter, not a location
function up(){if(cur==='/')return; go(cur.replace(/\/[^/]*\/?$/,'')||'/');}
async function edit(p){
  if(!isText(p.split('/').pop())){alert('Not a text file - use Download.');return;}
  $('editarea').value=await (await fetch('/api/read?path='+esc(p))).text();
  $('editname').textContent=p;editing=p;$('editor').classList.remove('hidden');
  // One history entry at the same hash, so Back closes the modal instead of
  // navigating the listing out from under it.
  if(!edPushed){history.pushState({p:cur,ed:1},'',location.hash);edPushed=true;}
}
function hideEd(){edPushed=false;$('editor').classList.add('hidden');}
function closeEd(){const pop=edPushed;hideEd();if(pop)history.back();}
window.addEventListener('popstate',e=>{
  if(editorOpen()){hideEd();return;}
  const p=(e.state&&e.state.p)||hashPath();
  if(p!==cur){cur=p;load();}        // unchanged path = the entry closeEd() consumed
});
// A failed write leaves the textarea holding the only copy of the edit, so the
// editor stays open on error - closing it would throw the work away silently.
async function save(){
  let r;
  try{r=await fetch('/api/save?path='+esc(editing),{method:'POST',body:$('editarea').value});}
  catch(e){alert('Save failed: '+e.message);return;}
  if(!r.ok){alert('Save failed: '+(await r.text()||r.status));return;}
  closeEd();load();
}
async function post(u){const r=await fetch(u,{method:'POST'});const t=await r.text();if(!r.ok)alert(t);load();return r.ok;}
async function del(p,isdir){
  const msg=isdir?'Delete the folder '+p+' AND EVERYTHING INSIDE IT?':'Delete '+p+'?';
  if(!confirm(msg))return;
  await post('/api/delete?path='+esc(p)+(isdir?'&recursive=1':''));
}
async function ren(p){
  const old=p.split('/').pop();
  const n=prompt('Rename to:',old);
  if(n===null)return;                       // cancelled
  const t=n.trim();
  if(!t||t===old)return;
  await post('/api/rename?path='+esc(p)+'&name='+esc(t));
}
// --- upload (button or drag-and-drop) ---------------------------------------
function banner(msg){$('dropmsg').textContent=msg;$('drop').classList.remove('hidden');}
function hideBanner(){$('drop').classList.add('hidden');}
async function sendOne(f,dir){
  const fd=new FormData(); fd.append('file',f,f.name);
  const r=await fetch('/api/upload?dir='+esc(dir),{method:'POST',body:fd});
  if(!r.ok) throw new Error(f.name+': '+await r.text());
}
// Uploads land in the folder that was open when the drop happened, even if the
// listing is navigated away mid-transfer.
async function upload(files,extra){
  const list=[...(files||[])];
  if(!list.length){hideBanner();return;}
  const dir=cur, errs=[];
  for(let i=0;i<list.length;i++){
    banner(`Uploading ${i+1} of ${list.length} to ${dir}\n${list[i].name}`);
    try{await sendOne(list[i],dir);}catch(e){errs.push(e.message);}
  }
  hideBanner();
  $('fileinput').value='';
  if(extra) errs.push(extra);
  if(errs.length) alert(errs.join('\n'));
  load();
}

// --- drag and drop ----------------------------------------------------------
// Only file drags are hijacked, and never while the editor is open (a drag in
// there is someone moving text around, not uploading).
const editorOpen=()=>!$('editor').classList.contains('hidden');
const dropping=e=>e.dataTransfer&&[...e.dataTransfer.types||[]].includes('Files')&&!editorOpen();
let dragDepth=0;   // dragenter/leave also fire for child elements
document.addEventListener('dragenter',e=>{
  if(!dropping(e))return; e.preventDefault();
  if(++dragDepth===1) banner('Drop files to upload into '+cur);
});
document.addEventListener('dragover',e=>{
  if(!dropping(e))return; e.preventDefault(); e.dataTransfer.dropEffect='copy';
});
document.addEventListener('dragleave',e=>{
  if(!dropping(e))return; if(--dragDepth<=0){dragDepth=0;hideBanner();}
});
document.addEventListener('drop',e=>{
  if(!dropping(e))return;
  e.preventDefault(); dragDepth=0;
  // webkitGetAsEntry/getAsFile must run synchronously, before any await:
  // the DataTransfer is cleared once the handler yields.
  const items=[...(e.dataTransfer.items||[])];
  const files=[]; let dirs=0;
  if(items.length&&items[0].webkitGetAsEntry){
    for(const it of items){
      if(it.kind!=='file')continue;
      const en=it.webkitGetAsEntry();
      if(en&&en.isDirectory){dirs++;continue;}   // folders need a recursive walk
      const f=it.getAsFile(); if(f) files.push(f);
    }
  }else{
    files.push(...(e.dataTransfer.files||[]));
  }
  upload(files,dirs?`Skipped ${dirs} folder(s) — drop files, not folders.`:'');
});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeEd();});
cur=hashPath(); load();      // load()'s replaceState seeds the first entry

</script>
</body></html>)HTMLDOC";

// Normalizes a `path`/`dir` query arg to an absolute SD path (leading slash).
static String arg_path(const char* key = "path") {
    String p = s_server.hasArg(key) ? s_server.arg(key) : String("/");
    if (p.length() == 0) p = "/";
    if (p[0] != '/') p = "/" + p;
    return p;
}

static String basename_of(const String& path) {
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

static void handle_index(void) { s_server.send_P(200, "text/html", INDEX_HTML); }

static void handle_list(void) {
    String path = arg_path();
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        s_server.send(400, "application/json", "{\"error\":\"not a directory\"}");
        return;
    }
    JsonDocument doc;
    doc["path"] = path;
    JsonArray arr = doc["entries"].to<JsonArray>();
    File e;
    while ((e = dir.openNextFile())) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = String(e.name());
        o["dir"] = e.isDirectory();
        o["size"] = (uint32_t)e.size();
        e.close();
    }
    dir.close();
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_read(void) {
    String path = arg_path();
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        s_server.send(404, "text/plain", "not found");
        return;
    }
    s_server.streamFile(f, "text/plain");
    f.close();
}

static void handle_download(void) {
    String path = arg_path();
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        s_server.send(404, "text/plain", "not found");
        return;
    }
    s_server.sendHeader("Content-Disposition",
                        "attachment; filename=\"" + basename_of(path) + "\"");
    s_server.streamFile(f, "application/octet-stream");
    f.close();
}

static void handle_save(void) {
    String path = arg_path();
    String body = s_server.hasArg("plain") ? s_server.arg("plain") : String("");
    File f = SD_MMC.open(path, FILE_WRITE, true);
    if (!f) {
        s_server.send(500, "text/plain", "open failed");
        return;
    }
    size_t n = f.write((const uint8_t*)body.c_str(), body.length());
    f.close();
    bool ok = (n == body.length());
    if (ok) s_writes++;
    s_server.send(ok ? 200 : 500, "text/plain", ok ? "saved" : "write failed");
}

// Deleting a directory takes everything under it, so it needs an explicit
// `recursive=1` from the caller — the UI only sends it after a confirm that
// spells that out.
static void handle_delete(void) {
    String path = arg_path();
    if (sd_tree_is_dir(path.c_str()) && !s_server.hasArg("recursive")) {
        s_server.send(400, "text/plain", "that is a folder — pass recursive=1 to delete it");
        return;
    }
    char err[80] = "delete failed";
    sd_tree_stats_t st = {0, 0};
    if (!sd_tree_remove(path.c_str(), &st, err, sizeof(err))) {
        s_server.send(500, "text/plain", err);
        return;
    }
    s_writes++;
    char msg[48];
    snprintf(msg, sizeof(msg), "deleted %d item%s", st.removed, st.removed == 1 ? "" : "s");
    s_server.send(200, "text/plain", msg);
}

// Renames a file or folder in place; `name` is a bare name, not a path.
static void handle_rename(void) {
    String path = arg_path();
    String name = s_server.hasArg("name") ? s_server.arg("name") : String("");
    char err[80] = "rename failed";
    if (!sd_tree_rename(path.c_str(), name.c_str(), err, sizeof(err))) {
        s_server.send(400, "text/plain", err);
        return;
    }
    s_writes++;
    s_server.send(200, "text/plain", "renamed");
}

// The browser sends a bare filename, but it is still client input: keep only
// the part after any separator so an upload can't climb out of `dir`.
static String upload_basename(const String& name) {
    int cut = 0;
    for (int i = 0; i < (int)name.length(); i++) {
        if (name[i] == '/' || name[i] == '\\') cut = i + 1;
    }
    String base = name.substring(cut);  // "" when the name ended in a separator
    if (base == "." || base == "..") return String("");
    return base;
}

static void handle_upload_done(void) {
    if (s_upload_ok) {
        s_server.send(200, "text/plain", "uploaded");
    } else {
        s_server.send(500, "text/plain", "could not write the file to the card");
    }
}

static void handle_upload_data(void) {
    HTTPUpload& up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        String dir = s_server.hasArg("dir") ? s_server.arg("dir") : String("/");
        if (dir.length() == 0) dir = "/";
        if (!dir.endsWith("/")) dir += "/";
        String name = upload_basename(up.filename);
        s_upload_ok = name.length() > 0;
        if (!s_upload_ok) {
            ESP_LOGE(TAG, "upload rejected: empty filename");
            return;
        }
        String path = dir + name;
        s_upload = SD_MMC.open(path, FILE_WRITE, true);
        s_upload_ok = (bool)s_upload;
        ESP_LOGI(TAG, "upload -> %s%s", path.c_str(), s_upload_ok ? "" : " (open failed)");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upload && s_upload.write(up.buf, up.currentSize) != up.currentSize) {
            s_upload_ok = false;  // card full or write error
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_upload) {
            s_upload.close();
            s_writes++;
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (s_upload) {
            s_upload.close();
            s_writes++;  // a partial file is still a change to the card
        }
        s_upload_ok = false;
    }
}

// The task owns the socket for its whole life, including the teardown: end()
// only raises s_stop, so nothing outside ever touches s_server concurrently.
// handleClient() serves at most one request per pass and delays 1 ms itself
// when idle; the extra tick keeps the task from monopolising a core while a
// client is connected but slow.
static void server_task(void*) {
    while (!s_stop) {
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_server.stop();
    // The stack figure is the point of logging here: TASK_STACK is an estimate
    // until a real transfer has run on the device.
    ESP_LOGI(TAG, "file server stopped (stack headroom %u B)",
             (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    s_task_alive = false;
    xSemaphoreGive(s_exited);  // nothing below this touches shared state
    vTaskDelete(nullptr);
}

// Waits for a previous task to finish winding down. True when none is left.
static bool await_task_exit(uint32_t timeout_ms) {
    if (!s_task_alive) return true;
    return xSemaphoreTake(s_exited, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void file_server_begin(void) {
    if (s_running) return;
    if (!s_exited) s_exited = xSemaphoreCreateBinary();
    if (!s_exited) {
        ESP_LOGE(TAG, "could not create the exit semaphore");
        return;
    }
    // A restart right after a stop can still find the old task serving its last
    // request; it must be gone before this one binds the port again.
    if (!await_task_exit(TEARDOWN_WAIT_MS)) {
        ESP_LOGE(TAG, "previous server task still running, not restarting");
        return;
    }
    // on() appends to the handler chain, so re-registering on every restart
    // would grow it without bound.
    if (!s_routes_registered) {
        s_server.on("/", HTTP_GET, handle_index);
        s_server.on("/api/list", HTTP_GET, handle_list);
        s_server.on("/api/read", HTTP_GET, handle_read);
        s_server.on("/api/download", HTTP_GET, handle_download);
        s_server.on("/api/save", HTTP_POST, handle_save);
        s_server.on("/api/delete", HTTP_POST, handle_delete);
        s_server.on("/api/rename", HTTP_POST, handle_rename);
        s_server.on("/api/upload", HTTP_POST, handle_upload_done, handle_upload_data);
        s_server.onNotFound([]() { s_server.send(404, "text/plain", "not found"); });
        s_routes_registered = true;
    }
    s_server.begin();
    s_stop = false;
    xSemaphoreTake(s_exited, 0);  // drop a give left by a stop that timed out
    s_task_alive = true;
    if (xTaskCreate(server_task, "fileserv", TASK_STACK, nullptr, TASK_PRIO, nullptr) != pdPASS) {
        s_task_alive = false;
        s_server.stop();
        ESP_LOGE(TAG, "could not start the server task");
        return;
    }
    s_running = true;
    ESP_LOGI(TAG, "file server started on :80 (own task)");
}

void file_server_end(void) {
    if (!s_running) return;
    s_running = false;
    s_stop = true;
    // Bounded so stopping the AP can't freeze the UI behind a slow transfer;
    // the task still closes the socket itself once its request completes.
    if (!await_task_exit(TEARDOWN_WAIT_MS)) {
        ESP_LOGW(TAG, "server task still finishing a request, winding down in the background");
    }
}

bool file_server_running(void) { return s_running; }

uint32_t file_server_write_count(void) { return s_writes; }
