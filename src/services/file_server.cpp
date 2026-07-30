#include "services/file_server.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WebServer.h>

#include "esp32-hal-log.h"
#include "storage/sd_tree.h"

static const char* TAG = "fileserver";

static WebServer s_server(80);
static bool s_running = false;
static File s_upload;

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
      <input id="fileinput" type="file" class="hidden" onchange="upload(this.files[0])">
    </div>
  </div>
  <div id="crumb" class="text-slate-400 font-mono text-sm mb-2">/</div>
  <div class="bg-slate-800 rounded-lg shadow p-3">
    <table><thead><tr><th>Name</th><th>Size</th><th>Actions</th></tr></thead>
    <tbody id="rows"></tbody></table>
  </div>
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
let cur='/', editing='', showHidden=false;
const FOLDER='<svg viewBox="0 0 20 20" width="15" height="15" fill="#eab308" style="vertical-align:-2px"><path d="M2 6a2 2 0 0 1 2-2h3l2 2h7a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6z"/></svg>';
const $=id=>document.getElementById(id);
const esc=s=>encodeURIComponent(s);
const q=s=>String(s).replace(/'/g,"\\'");
const join=(d,n)=>d.endsWith('/')?d+n:d+'/'+n;
const fmt=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';
async function load(){
  const d=await (await fetch('/api/list?path='+esc(cur))).json();
  cur=d.path; $('crumb').textContent=cur;
  const rows=$('rows'); rows.innerHTML='';
  let items=(d.entries||[]);
  if(!showHidden) items=items.filter(e=>!e.name.startsWith('.'));  // dotfiles hidden by default
  items.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).forEach(e=>{
    const full=join(cur,e.name);
    const nm=e.dir?`<span class="name" onclick="cd('${q(full)}')">${FOLDER} ${e.name}</span>`:e.name;
    const fileAct=e.dir?'':`<button class="btn btn-slate" onclick="edit('${q(full)}')">Edit</button>
      <a class="btn btn-blue" href="/api/download?path=${esc(full)}">Download</a>`;
    const act=`${fileAct}
      <button class="btn btn-slate" onclick="ren('${q(full)}')">Rename</button>
      <button class="btn btn-red" onclick="del('${q(full)}',${e.dir?1:0})">Delete</button>`;
    const tr=document.createElement('tr');
    tr.innerHTML=`<td class="truncate">${nm}</td><td class="text-slate-400">${e.dir?'':fmt(e.size)}</td><td class="flex gap-2 wrap">${act}</td>`;
    rows.appendChild(tr);
  });
}
function cd(p){cur=p;load();}
function toggleHidden(v){showHidden=v;load();}
function up(){if(cur==='/')return; cur=cur.replace(/\/[^/]*\/?$/,'')||'/'; load();}
async function edit(p){$('editarea').value=await (await fetch('/api/read?path='+esc(p))).text();$('editname').textContent=p;editing=p;$('editor').classList.remove('hidden');}
function closeEd(){$('editor').classList.add('hidden');}
async function save(){await fetch('/api/save?path='+esc(editing),{method:'POST',body:$('editarea').value});closeEd();load();}
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
async function upload(f){if(!f)return;const fd=new FormData();fd.append('file',f);await fetch('/api/upload?dir='+esc(cur),{method:'POST',body:fd});$('fileinput').value='';load();}
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeEd();});
load();
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
    s_server.send(200, "text/plain", "renamed");
}

static void handle_upload_done(void) { s_server.send(200, "text/plain", "uploaded"); }

static void handle_upload_data(void) {
    HTTPUpload& up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        String dir = s_server.hasArg("dir") ? s_server.arg("dir") : String("/");
        if (dir.length() == 0) dir = "/";
        if (!dir.endsWith("/")) dir += "/";
        String path = dir + up.filename;
        s_upload = SD_MMC.open(path, FILE_WRITE, true);
        ESP_LOGI(TAG, "upload -> %s", path.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upload) s_upload.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_upload) s_upload.close();
    }
}

void file_server_begin(void) {
    if (s_running) return;
    s_server.on("/", HTTP_GET, handle_index);
    s_server.on("/api/list", HTTP_GET, handle_list);
    s_server.on("/api/read", HTTP_GET, handle_read);
    s_server.on("/api/download", HTTP_GET, handle_download);
    s_server.on("/api/save", HTTP_POST, handle_save);
    s_server.on("/api/delete", HTTP_POST, handle_delete);
    s_server.on("/api/rename", HTTP_POST, handle_rename);
    s_server.on("/api/upload", HTTP_POST, handle_upload_done, handle_upload_data);
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "not found"); });
    s_server.begin();
    s_running = true;
    ESP_LOGI(TAG, "file server started on :80");
}

void file_server_end(void) {
    if (!s_running) return;
    s_server.stop();
    s_running = false;
    ESP_LOGI(TAG, "file server stopped");
}

void file_server_handle(void) {
    if (s_running) s_server.handleClient();
}

bool file_server_running(void) { return s_running; }
