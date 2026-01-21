#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 8080
#define BUF_SIZE 262144
#define MAX_METRICS 4096
#define THREAD_POOL_SIZE 24
#define BACKLOG 128
#define QMAX 2048

// Theme
static const char *CORE_STYLE =
"<style>"
":root{"
" --bg:#0b0f17;"
" --glass:rgba(30,40,70,.28);"
" --accent:#00f0d0;"
" --txt:#e0f0ff;"
" --txt-dim:#a0c0e0;"
" --border:rgba(0,240,208,.14);"
" --radius:16px;"
" --shadow:0 12px 32px rgba(0,0,0,.55);"
" --transition:all .28s cubic-bezier(.4,0,.2,1);"
"}"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh;line-height:1.6;"
"background-image:radial-gradient(circle at 4px 4px, rgba(0,240,208,0.06) 1px, transparent 0);"
"background-size:40px 40px;}"
".wrap{max-width:1280px;margin:0 auto;padding:2.5rem 1.25rem;}"
"h1{font-size:2.5rem;font-weight:900;letter-spacing:-.03em}"
"h2{font-size:1.75rem;font-weight:800;margin-bottom:1rem}"
"h3{font-size:1.2rem;font-weight:800;margin-bottom:.3rem}"
"a{color:var(--accent);text-decoration:none;transition:var(--transition)}"
"a:hover{color:#fff;text-shadow:0 0 12px rgba(0,240,208,.18)}"
".glass{background:var(--glass);backdrop-filter:blur(16px) saturate(180%);-webkit-backdrop-filter:blur(16px) saturate(180%);"
"border:1px solid var(--border);border-radius:var(--radius);box-shadow:var(--shadow);transition:var(--transition);padding:1.5rem}"
".glass:hover{transform:translateY(-6px) scale(1.012);box-shadow:0 24px 48px rgba(0,240,208,.20);border-color:var(--accent)}"
".topbar{display:flex;align-items:center;justify-content:space-between;gap:1rem;margin-bottom:1.75rem;}"
".pill{display:inline-flex;align-items:center;gap:.6rem;padding:.55rem .9rem;border:1px solid var(--border);border-radius:999px;background:rgba(0,240,208,.06)}"
".muted{color:var(--txt-dim)}"
".kpi{display:grid;grid-template-columns:repeat(3,1fr);gap:1rem;margin:1.75rem 0 2.25rem}"
"@media (max-width:860px){.kpi{grid-template-columns:1fr}}"
".kpi .v{font-size:1.6rem;font-weight:1000;color:var(--accent)}"
".card-grid{display:grid;grid-template-columns:1fr;gap:1.25rem;margin-top:2rem}"
"@media (min-width:640px){.card-grid{grid-template-columns:repeat(2,1fr)}}"
"@media (min-width:1024px){.card-grid{grid-template-columns:repeat(3,1fr)}}"
".tag{font-size:.72rem;font-weight:900;letter-spacing:.1em;text-transform:uppercase;color:var(--accent);margin-bottom:.45rem;display:block}"
"button{background:linear-gradient(145deg,var(--accent),#00c0a8);color:#0a1421;font-weight:1000;padding:.85rem 1.6rem;border:none;border-radius:12px;cursor:pointer;"
"transition:var(--transition);box-shadow:0 6px 20px rgba(0,240,208,.22)}"
"button:hover:not(:disabled){transform:translateY(-2px) scale(1.03);box-shadow:0 12px 32px rgba(0,240,208,.36)}"
"button:disabled{opacity:.5;cursor:not-allowed;box-shadow:none}"
".btn-ghost{background:transparent;color:var(--txt);border:1px solid var(--border);box-shadow:none}"
".btn-ghost:hover{background:rgba(255,255,255,.04)}"
"table{width:100%;border-collapse:collapse;margin:1.25rem 0}"
"th,td{padding:.95rem;text-align:left;border-bottom:1px solid var(--border)}"
"th{background:rgba(0,240,208,.08);font-weight:900}"
".shell{max-width:980px;margin:1.25rem auto;text-align:center}"
".hud{display:flex;justify-content:center;gap:.85rem;flex-wrap:wrap;margin:1.25rem 0}"
".hud .glass{padding:.85rem 1rem;min-width:160px}"
".big{font-size:2.2rem;font-weight:1200;color:var(--accent)}"
".footer-note{margin-top:5rem;text-align:center;font-size:.9rem;opacity:.6}"
"</style>";

// Metrics
typedef struct {
    char game[32];
    char time_s[32];
    char score[96];
} Metric;

static Metric metrics[MAX_METRICS];
static int metric_count = 0;
static CRITICAL_SECTION metrics_cs;

static void now_string(char out[32]) {
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_MSC_VER)
    localtime_s(&tmv, &t);
#else
    struct tm *ptm = localtime(&t);
    if (ptm) tmv = *ptm; else memset(&tmv, 0, sizeof(tmv));
#endif
    strftime(out, 32, "%Y-%m-%d %H:%M:%S", &tmv);
}

static int icmp_char(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return (unsigned char)a - (unsigned char)b;
}

static int icmp(const char *a, const char *b) {
    while (*a && *b) {
        int d = icmp_char(*a, *b);
        if (d) return d;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int nicmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (!ca || !cb) return (unsigned char)ca - (unsigned char)cb;
        int d = icmp_char(ca, cb);
        if (d) return d;
    }
    return 0;
}

static void url_decode(char *dst, size_t dstsz, const char *src) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dstsz; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            int v = (int)strtol(hex, NULL, 16);
            dst[di++] = (char)v;
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = 0;
}

static void normalize_score(char *s, size_t cap) {
    if (!s || !*s) return;
    if (strchr(s, '%') || strchr(s, '+')) {
        char tmp[128] = {0};
        url_decode(tmp, sizeof(tmp), s);
        strncpy(s, tmp, cap - 1);
        s[cap - 1] = 0;
    }
    for (;;) {
        char *p = strstr(s, "%20");
        if (!p) break;
        *p = ' ';
        memmove(p + 1, p + 3, strlen(p + 3) + 1);
    }
}

static void save_metrics_locked(void) {
    FILE *f = fopen("metrics.txt", "w");
    if (!f) return;
    for (int i = 0; i < metric_count; i++) {
        fprintf(f, "%s|%s|%s\n", metrics[i].game, metrics[i].time_s, metrics[i].score);
    }
    fclose(f);
}

static void load_metrics(void) {
    FILE *f = fopen("metrics.txt", "r");
    if (!f) return;

    EnterCriticalSection(&metrics_cs);
    metric_count = 0;

    char line[256];
    while (metric_count < MAX_METRICS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        Metric m = (Metric){0};
        if (sscanf(line, "%31[^|]|%31[^|]|%95[^\n]", m.game, m.time_s, m.score) == 3) {
            normalize_score(m.score, sizeof(m.score));
            metrics[metric_count++] = m;
        }
    }
    LeaveCriticalSection(&metrics_cs);

    fclose(f);
}

static void record_metric(const char *game, const char *score) {
    EnterCriticalSection(&metrics_cs);
    if (metric_count < MAX_METRICS) {
        Metric *m = &metrics[metric_count++];
        strncpy(m->game, game, 31);  m->game[31] = 0;
        now_string(m->time_s);
        strncpy(m->score, score, 95); m->score[95] = 0;
        normalize_score(m->score, sizeof(m->score));
        save_metrics_locked();
    }
    LeaveCriticalSection(&metrics_cs);
}

static void best_for_game(const char *game, char out[96]) {
    out[0] = 0;

    EnterCriticalSection(&metrics_cs);

    int found = 0;
    double best = 0;
    int best_is_low = 0;
    char best_str[96] = {0};

    for (int i = 0; i < metric_count; i++) {
        if (icmp(metrics[i].game, game) != 0) continue;
        const char *s = metrics[i].score;

        double val = 0;
        int ok = (sscanf(s, "%lf", &val) == 1);
        int is_ms = (strstr(s, "ms") != NULL);

        if (!found) {
            found = 1;
            best_is_low = is_ms ? 1 : 0;
            best = ok ? val : 0;
            strncpy(best_str, s, 95);
            best_str[95] = 0;
        } else {
            if (ok) {
                if (best_is_low) {
                    if (is_ms && val < best) { best = val; strncpy(best_str, s, 95); best_str[95] = 0; }
                } else {
                    if (!is_ms && val > best) { best = val; strncpy(best_str, s, 95); best_str[95] = 0; }
                }
            } else {
                strncpy(best_str, s, 95);
                best_str[95] = 0;
            }
        }
    }

    LeaveCriticalSection(&metrics_cs);

    if (found) strncpy(out, best_str, 95);
    else strncpy(out, "—", 95);
    out[95] = 0;

    normalize_score(out, 96);
}

// Query
static const char* get_qs_param(const char *qs, const char *key, char *out, size_t outsz) {
    out[0] = 0;
    if (!qs || !*qs) return NULL;

    size_t klen = strlen(key);
    const char *p = qs;

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);

        const char *eq = memchr(p, '=', seglen);
        if (eq) {
            size_t namelen = (size_t)(eq - p);
            if (namelen == klen && nicmp(p, key, klen) == 0) {
                char tmp[512] = {0};
                size_t vlen = seglen - namelen - 1;
                if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
                memcpy(tmp, eq + 1, vlen);
                tmp[vlen] = 0;
                url_decode(out, outsz, tmp);
                return out;
            }
        }

        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

// HTTP
static int send_all(SOCKET s, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static char *http_response(const char *ctype, const char *body, int code, const char *status) {
    size_t blen = body ? strlen(body) : 0;
    char *resp = (char*)malloc(BUF_SIZE);
    if (!resp) return NULL;
    int n = snprintf(resp, BUF_SIZE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n"
        "%s",
        code, status, ctype, blen, body ? body : "");
    if (n < 0) { free(resp); return NULL; }
    return resp;
}

static char *build_page(const char *title, const char *content_html) {
    char *body = (char*)malloc(BUF_SIZE);
    if (!body) return http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");

    snprintf(body, BUF_SIZE,
        "<!DOCTYPE html><html lang=\"en\"><head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>%s • Cognitive Arena</title>"
        "%s"
        "</head><body>%s</body></html>",
        title, CORE_STYLE, content_html);

    char *resp = http_response("text/html; charset=UTF-8", body, 200, "OK");
    free(body);
    return resp;
}

// Layout
static void append_topbar(char *p, size_t cap) {
    strncat(p,
        "<div class=\"wrap\">"
        "<div class=\"topbar\">"
        " <div class=\"pill\"><span style=\"color:var(--accent);font-weight:1200;\">Cognitive Arena</span>"
        "  <span class=\"muted\" id=\"clock\"></span></div>"
        " <a href=\"/\"><button class=\"btn-ghost\">← Dashboard</button></a>"
        "</div>"
        "<script>"
        "const clk=()=>{document.getElementById('clock').textContent=new Date().toLocaleString();};"
        "clk();setInterval(clk,1000);"
        "</script>",
        cap - strlen(p) - 1
    );
}

static void append_footer(char *p, size_t cap) {
    strncat(p,
        "<div class=\"footer-note\">Cognitive Arena - 2026</div></div>",
        cap - strlen(p) - 1
    );
}

static void append_helpers(char *p, size_t cap) {
    strncat(p,
        "<script>"
        "function decodeBest(v){"
        "  if(!v) return '—';"
        "  try{"
        "    if(v.includes('%') || v.includes('+')) return decodeURIComponent(v.replace(/\\+/g,' '));"
        "  }catch(e){}"
        "  return v.replace(/%20/g,' ');"
        "}"
        "async function record(game, score){"
        "  try{await fetch(`/record?game=${encodeURIComponent(game)}&score=${encodeURIComponent(score)}`,{cache:'no-store'});}catch(e){}"
        "}"
        "async function loadBest(game, elId){"
        "  try{"
        "    const r=await fetch(`/best.json?game=${encodeURIComponent(game)}`,{cache:'no-store'});"
        "    const j=await r.json();"
        "    const el=document.getElementById(elId); if(el) el.textContent=decodeBest(j.best);"
        "  }catch(e){"
        "    const el=document.getElementById(elId); if(el) el.textContent='—';"
        "  }"
        "}"
        "</script>",
        cap - strlen(p) - 1
    );
}

// Games
static char *build_reaction_html(void) {
    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;
    append_topbar(p, BUF_SIZE);

    strncat(p,
        "<div class=\"glass shell\">"
        "<span class=\"tag\">Reflex</span>"
        "<h1>⚡ Reaction Speed</h1>"
        "<p class=\"muted\">3 trials. Click only when the box turns cyan. False starts count.</p>"
        "<div class=\"hud\">"
        " <div class=\"glass\"><div class=\"muted\">Trial</div><div class=\"big\" id=\"trial\">1/3</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Best</div><div class=\"big\" id=\"best\">—</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Last</div><div class=\"big\" id=\"last\">—</div></div>"
        "</div>"
        "<div id=\"box\" style=\"width:240px;height:240px;background:#2a0f18;margin:2.2rem auto;border-radius:1.5rem;"
        "cursor:pointer;transition:background .14s;user-select:none;\"></div>"
        "<div style=\"display:flex;justify-content:center;gap:.8rem;margin-top:1.25rem;\">"
        " <button id=\"restart\">Restart</button>"
        "</div>"
        "<p id=\"msg\" class=\"muted\" style=\"margin-top:1.25rem;font-size:1.15rem;\"></p>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    append_helpers(p, BUF_SIZE);

    strncat(p,
        "<script>"
        "loadBest('Reaction','best');"
        "let startT=0, armed=false, timer=0, trials=[], maxT=3, trial=1;"
        "const box=document.getElementById('box');"
        "const msg=document.getElementById('msg');"
        "const trialEl=document.getElementById('trial');"
        "const lastEl=document.getElementById('last');"
        "function setTrial(){trialEl.textContent=`${trial}/${maxT}`;}"
        "function arm(){"
        " clearTimeout(timer);"
        " armed=false; startT=0;"
        " box.style.background='#2a0f18';"
        " msg.textContent='Get ready…';"
        " const delay=900+Math.random()*2200;"
        " timer=setTimeout(()=>{"
        "   box.style.background='#00f0d0';"
        "   startT=performance.now();"
        "   armed=true;"
        "   msg.textContent='NOW!';"
        " },delay);"
        "}"
        "function finish(){"
        " const avg=Math.round(trials.reduce((a,b)=>a+b,0)/trials.length);"
        " msg.textContent=`Done. Average: ${avg} ms`;"
        " lastEl.textContent=`${avg} ms`;"
        " record('Reaction', `${avg} ms avg`);"
        " setTimeout(()=>loadBest('Reaction','best'), 250);"
        "}"
        "box.addEventListener('click',()=>{"
        " if(!armed){"
        "  trials.push(999);"
        "  lastEl.textContent='False (+999)';"
        "  msg.textContent='False start. Next trial…';"
        " }else{"
        "  const ms=Math.max(1, Math.round(performance.now()-startT));"
        "  trials.push(ms);"
        "  lastEl.textContent=`${ms} ms`;"
        "  msg.textContent=`${ms} ms`;"
        " }"
        " armed=false; startT=0;"
        " if(trials.length>=maxT){ finish(); return; }"
        " trial++; setTrial();"
        " setTimeout(arm, 650);"
        "});"
        "document.getElementById('restart').onclick=()=>{"
        " trials=[]; trial=1; setTrial(); msg.textContent=''; lastEl.textContent='—'; arm();"
        "};"
        "setTrial(); arm();"
        "</script>",
        BUF_SIZE - strlen(p) - 1
    );

    append_footer(p, BUF_SIZE);
    return p;
}

static char *build_velocity_html(void) {
    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;
    append_topbar(p, BUF_SIZE);

    strncat(p,
        "<div class=\"glass shell\">"
        "<span class=\"tag\">Speed</span>"
        "<h1>🖱 Click Velocity</h1>"
        "<p class=\"muted\">10 seconds. Click inside the pad only.</p>"
        "<div class=\"hud\">"
        " <div class=\"glass\"><div class=\"muted\">Time Left</div><div class=\"big\" id=\"t\">10.0</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Clicks</div><div class=\"big\" id=\"c\">0</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Best</div><div class=\"big\" id=\"best\">—</div></div>"
        "</div>"
        "<div id=\"pad\" style=\"width:min(760px,92vw);height:340px;margin:1.2rem auto;border-radius:18px;"
        "background:rgba(0,240,208,.06);border:1px solid var(--border);display:flex;align-items:center;justify-content:center;"
        "user-select:none;cursor:pointer;\">"
        "<div class=\"muted\" id=\"status\" style=\"font-size:1.25rem;\">Press Start</div>"
        "</div>"
        "<div style=\"display:flex;justify-content:center;gap:.8rem;margin-top:1.0rem;\">"
        " <button id=\"start\">Start</button>"
        " <button class=\"btn-ghost\" id=\"reset\">Reset</button>"
        "</div>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    append_helpers(p, BUF_SIZE);

    strncat(p,
        "<script>"
        "loadBest('Velocity','best');"
        "let active=false, clicks=0, startT=0, raf=0;"
        "const tEl=document.getElementById('t');"
        "const cEl=document.getElementById('c');"
        "const pad=document.getElementById('pad');"
        "const statusEl=document.getElementById('status');"
        "function tick(){"
        " if(!active) return;"
        " const left=Math.max(0, 10 - (performance.now()-startT)/1000);"
        " tEl.textContent=left.toFixed(1);"
        " if(left<=0){"
        "  active=false;"
        "  statusEl.textContent=`Done: ${clicks} clicks`;"
        "  record('Velocity', `${clicks} clicks`);"
        "  setTimeout(()=>loadBest('Velocity','best'),250);"
        "  return;"
        " }"
        " raf=requestAnimationFrame(tick);"
        "}"
        "document.getElementById('start').onclick=()=>{"
        " if(active) return;"
        " clicks=0; cEl.textContent='0';"
        " active=true; startT=performance.now();"
        " statusEl.textContent='GO';"
        " cancelAnimationFrame(raf);"
        " raf=requestAnimationFrame(tick);"
        "};"
        "document.getElementById('reset').onclick=()=>{"
        " active=false; clicks=0; cEl.textContent='0'; tEl.textContent='10.0'; statusEl.textContent='Press Start';"
        "};"
        "pad.addEventListener('click',()=>{ if(!active) return; clicks++; cEl.textContent=String(clicks); });"
        "</script>",
        BUF_SIZE - strlen(p) - 1
    );

    append_footer(p, BUF_SIZE);
    return p;
}

static char *build_aim_html(void) {
    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;
    append_topbar(p, BUF_SIZE);

    strncat(p,
        "<div class=\"glass shell\">"
        "<span class=\"tag\">Accuracy</span>"
        "<h1>🎯 Aim Trainer</h1>"
        "<p class=\"muted\">20 hits. Speed ramps up. Records completion time.</p>"
        "<div class=\"hud\">"
        " <div class=\"glass\"><div class=\"muted\">Hits</div><div class=\"big\" id=\"hits\">0/20</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Time</div><div class=\"big\" id=\"time\">0.00s</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Best</div><div class=\"big\" id=\"best\">—</div></div>"
        "</div>"
        "<canvas id=\"c\" width=\"860\" height=\"520\" style=\"background:#0d1525;border:1px solid #1a2a40;display:block;"
        "margin:1.2rem auto;border-radius:1rem;\"></canvas>"
        "<div style=\"display:flex;justify-content:center;gap:.8rem;margin-top:1.0rem;\">"
        " <button id=\"restart\">Restart</button>"
        "</div>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    append_helpers(p, BUF_SIZE);

    strncat(p,
        "<script>"
        "loadBest('AimTrainer','best');"
        "const c=document.getElementById('c');"
        "const ctx=c.getContext('2d');"
        "let hits=0, target={x:430,y:260,r:24,vx:4.2,vy:3.4};"
        "let startT=0, running=false, raf=0;"
        "const hitsEl=document.getElementById('hits');"
        "const timeEl=document.getElementById('time');"
        "function reset(){"
        " hits=0; target={x:430,y:260,r:24,vx:4.2,vy:3.4};"
        " hitsEl.textContent='0/20'; timeEl.textContent='0.00s';"
        " startT=0; running=true;"
        "}"
        "function draw(){"
        " if(!running) return;"
        " ctx.clearRect(0,0,c.width,c.height);"
        " target.x += target.vx; target.y += target.vy;"
        " if(target.x<target.r||target.x>c.width-target.r) target.vx=-target.vx;"
        " if(target.y<target.r||target.y>c.height-target.r) target.vy=-target.vy;"
        " ctx.fillStyle='#00f0d0';"
        " ctx.beginPath(); ctx.arc(target.x,target.y,target.r,0,Math.PI*2); ctx.fill();"
        " if(startT){"
        "  const s=(performance.now()-startT)/1000;"
        "  timeEl.textContent=s.toFixed(2)+'s';"
        " }"
        " raf=requestAnimationFrame(draw);"
        "}"
        "c.addEventListener('click',(e)=>{"
        " if(!running) return;"
        " const r=c.getBoundingClientRect();"
        " const mx=e.clientX-r.left, my=e.clientY-r.top;"
        " const d=Math.hypot(mx-target.x, my-target.y);"
        " if(d<=target.r+2){"
        "  if(!startT) startT=performance.now();"
        "  hits++;"
        "  hitsEl.textContent=hits+'/20';"
        "  target.vx*=1.06; target.vy*=1.06;"
        "  target.r=Math.max(14, target.r*0.985);"
        "  if(hits>=20){"
        "   running=false;"
        "   const s=((performance.now()-startT)/1000).toFixed(2);"
        "   timeEl.textContent=s+'s';"
        "   record('AimTrainer', `${s}s for 20 hits`);"
        "   setTimeout(()=>loadBest('AimTrainer','best'),250);"
        "  }"
        " }"
        "});"
        "document.getElementById('restart').onclick=()=>{ cancelAnimationFrame(raf); reset(); raf=requestAnimationFrame(draw); };"
        "reset(); raf=requestAnimationFrame(draw);"
        "</script>",
        BUF_SIZE - strlen(p) - 1
    );

    append_footer(p, BUF_SIZE);
    return p;
}

static char *build_memory_html(void) {
    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;
    append_topbar(p, BUF_SIZE);

    strncat(p,
        "<div class=\"glass shell\">"
        "<span class=\"tag\">Memory</span>"
        "<h1>🧠 Memory Tiles</h1>"
        "<p class=\"muted\">Watch the 5-tile sequence, then repeat it. One mistake ends the run.</p>"
        "<div class=\"hud\">"
        " <div class=\"glass\"><div class=\"muted\">Progress</div><div class=\"big\" id=\"prog\">0/5</div></div>"
        " <div class=\"glass\"><div class=\"muted\">State</div><div class=\"big\" id=\"state\">Ready</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Best</div><div class=\"big\" id=\"best\">—</div></div>"
        "</div>"
        "<div id=\"g\" style=\"display:grid;grid-template-columns:repeat(4,1fr);gap:1rem;max-width:480px;margin:1.6rem auto;\"></div>"
        "<div style=\"display:flex;justify-content:center;gap:.8rem;margin-top:1.0rem;\">"
        " <button id=\"start\">Start</button>"
        " <button class=\"btn-ghost\" id=\"reset\">Reset</button>"
        "</div>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    append_helpers(p, BUF_SIZE);

    strncat(p,
        "<script>"
        "loadBest('Memory','best');"
        "const g=document.getElementById('g');"
        "const progEl=document.getElementById('prog');"
        "const stateEl=document.getElementById('state');"
        "let tiles=[], seq=[], idx=0, canClick=false;"
        "for(let i=0;i<16;i++){"
        " const t=document.createElement('div');"
        " t.style.background='#1a2538';"
        " t.style.height='96px';"
        " t.style.borderRadius='14px';"
        " t.style.border='1px solid rgba(0,240,208,.10)';"
        " t.style.cursor='pointer';"
        " t.dataset.idx=i;"
        " t.onclick=onClick;"
        " tiles.push(t); g.appendChild(t);"
        "}"
        "function flash(el){"
        " el.style.background='#00f0d0';"
        " el.style.boxShadow='0 0 28px rgba(0,240,208,.35)';"
        " setTimeout(()=>{el.style.background='#1a2538'; el.style.boxShadow='none';},360);"
        "}"
        "function start(){"
        " seq=[]; idx=0; canClick=false;"
        " for(let i=0;i<5;i++) seq.push(Math.floor(Math.random()*16));"
        " progEl.textContent='0/5';"
        " stateEl.textContent='Watch';"
        " let k=0;"
        " const iv=setInterval(()=>{"
        "  flash(tiles[seq[k]]);"
        "  k++;"
        "  if(k>=seq.length){"
        "   clearInterval(iv);"
        "   stateEl.textContent='Repeat';"
        "   canClick=true;"
        "  }"
        " },650);"
        "}"
        "function fail(at){"
        " canClick=false;"
        " stateEl.textContent='Failed';"
        " record('Memory', `${at}/5`);"
        " setTimeout(()=>loadBest('Memory','best'),250);"
        "}"
        "function win(){"
        " canClick=false;"
        " stateEl.textContent='Perfect';"
        " record('Memory', '5/5');"
        " setTimeout(()=>loadBest('Memory','best'),250);"
        "}"
        "function onClick(e){"
        " if(!canClick) return;"
        " const id=+e.currentTarget.dataset.idx;"
        " flash(e.currentTarget);"
        " if(id!==seq[idx]){ fail(idx); return; }"
        " idx++;"
        " progEl.textContent=idx+'/5';"
        " if(idx>=5) win();"
        "}"
        "document.getElementById('start').onclick=start;"
        "document.getElementById('reset').onclick=()=>{ seq=[]; idx=0; canClick=false; progEl.textContent='0/5'; stateEl.textContent='Ready'; };"
        "</script>",
        BUF_SIZE - strlen(p) - 1
    );

    append_footer(p, BUF_SIZE);
    return p;
}

static char *build_typing_html(void) {
    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;
    append_topbar(p, BUF_SIZE);

    strncat(p,
        "<div class=\"glass shell\">"
        "<span class=\"tag\">Typing</span>"
        "<h1>⌨ Typing Burst</h1>"
        "<p class=\"muted\">30 seconds. Type words exactly. Records WPM + accuracy.</p>"
        "<div class=\"hud\">"
        " <div class=\"glass\"><div class=\"muted\">Time Left</div><div class=\"big\" id=\"t\">30</div></div>"
        " <div class=\"glass\"><div class=\"muted\">WPM</div><div class=\"big\" id=\"wpm\">—</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Best</div><div class=\"big\" id=\"best\">—</div></div>"
        "</div>"
        "<p id=\"w\" style=\"font-size:1.55rem;line-height:1.8;letter-spacing:.3px;min-height:120px;margin:1.2rem 0;"
        "padding:1.25rem;background:rgba(20,30,50,0.35);border:1px solid rgba(0,240,208,.10);border-radius:14px;\"></p>"
        "<input id=\"i\" style=\"font-size:1.35rem;width:100%;padding:1rem;border:none;border-radius:12px;background:#1a2538;color:#e0f0ff;"
        "outline:2px solid transparent;transition:var(--transition);\" autocomplete=\"off\" disabled>"
        "<p class=\"muted\" style=\"margin-top:1.1rem;font-size:1.1rem;\">Accuracy: <span id=\"acc\">—</span>% • Correct words: <span id=\"cw\">0</span></p>"
        "<div style=\"display:flex;justify-content:center;gap:.8rem;margin-top:1.0rem;\">"
        " <button id=\"start\">Start</button>"
        " <button class=\"btn-ghost\" id=\"reset\">Reset</button>"
        "</div>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    append_helpers(p, BUF_SIZE);

    strncat(p,
        "<script>"
        "loadBest('Typing','best');"
        "const words=('signal vector latency compose memory arena cognitive focus reaction velocity precision habit future glass neon system kernel thread network fast strict stable secure clean sharp').split(/\\s+/);"
        "function shuffle(a){for(let i=a.length-1;i>0;i--){const j=Math.floor(Math.random()*(i+1));[a[i],a[j]]=[a[j],a[i]];}return a;}"
        "let pool=shuffle(words.concat(words).concat(words));"
        "let idx=0, running=false, startT=0, timer=0;"
        "let typed=0, correct=0, correctChars=0;"
        "const wEl=document.getElementById('w');"
        "const iEl=document.getElementById('i');"
        "const tEl=document.getElementById('t');"
        "const wpmEl=document.getElementById('wpm');"
        "const accEl=document.getElementById('acc');"
        "const cwEl=document.getElementById('cw');"
        "function render(){"
        " const slice=pool.slice(idx, idx+10);"
        " const html=slice.map((w,j)=>j===0?`<span style=\"background:#00f0d0;color:#0a1421;padding:.2rem .55rem;border-radius:8px;font-weight:1000;\">${w}</span>`:w).join(' ');"
        " wEl.innerHTML=html;"
        "}"
        "function end(){"
        " running=false; clearInterval(timer);"
        " const mins=(performance.now()-startT)/60000;"
        " const wpm=Math.round((correctChars/5)/Math.max(0.001, mins));"
        " const acc=Math.round((correct/Math.max(1, typed))*100);"
        " wpmEl.textContent=String(wpm); accEl.textContent=String(acc);"
        " record('Typing', `${wpm} WPM / ${acc}%`);"
        " setTimeout(()=>loadBest('Typing','best'),250);"
        " iEl.disabled=true;"
        "}"
        "function hardReset(){"
        " idx=0; running=false; startT=0; typed=0; correct=0; correctChars=0;"
        " tEl.textContent='30'; wpmEl.textContent='—'; accEl.textContent='—'; cwEl.textContent='0';"
        " iEl.value=''; iEl.disabled=true;"
        " pool=shuffle(words.concat(words).concat(words));"
        " render();"
        "}"
        "document.getElementById('start').onclick=()=>{"
        " if(running) return;"
        " hardReset();"
        " running=true; iEl.disabled=false; iEl.focus();"
        " let left=30; tEl.textContent=String(left);"
        " startT=performance.now();"
        " timer=setInterval(()=>{ left--; tEl.textContent=String(left); if(left<=0) end(); },1000);"
        "};"
        "document.getElementById('reset').onclick=hardReset;"
        "iEl.addEventListener('keydown',(e)=>{"
        " if(!running) return;"
        " if(e.key===' ' || e.key==='Enter'){"
        "  e.preventDefault();"
        "  const v=iEl.value.trim();"
        "  if(!v) return;"
        "  typed++;"
        "  const target=pool[idx];"
        "  if(v===target){ correct++; correctChars += target.length; idx++; cwEl.textContent=String(correct); }"
        "  iEl.value='';"
        "  const acc=Math.round((correct/Math.max(1, typed))*100); accEl.textContent=String(acc);"
        "  render();"
        " }"
        "});"
        "hardReset();"
        "</script>",
        BUF_SIZE - strlen(p) - 1
    );

    append_footer(p, BUF_SIZE);
    return p;
}

static char *build_stroop_html(void) {
    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;
    append_topbar(p, BUF_SIZE);

    strncat(p,
        "<div class=\"glass shell\">"
        "<span class=\"tag\">Attention</span>"
        "<h1>🧩 Stroop Sprint</h1>"
        "<p class=\"muted\">Click the <b>COLOR OF THE TEXT</b>, not the word. 30 seconds.</p>"
        "<div class=\"hud\">"
        " <div class=\"glass\"><div class=\"muted\">Time Left</div><div class=\"big\" id=\"t\">30</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Correct</div><div class=\"big\" id=\"ok\">0</div></div>"
        " <div class=\"glass\"><div class=\"muted\">Best</div><div class=\"big\" id=\"best\">—</div></div>"
        "</div>"
        "<div class=\"glass\" style=\"max-width:720px;margin:1.2rem auto;\">"
        " <div class=\"muted\" style=\"margin-bottom:.6rem;\">Match the text color</div>"
        " <div id=\"stim\" style=\"font-size:3.2rem;font-weight:1100;letter-spacing:.06em;\">READY</div>"
        " <div class=\"muted\" id=\"msg\" style=\"margin-top:.75rem;min-height:1.5rem;\"></div>"
        "</div>"
        "<div style=\"display:flex;justify-content:center;gap:.7rem;flex-wrap:wrap;margin:1rem 0;\">"
        " <button class=\"btn-ghost\" id=\"bRed\"   style=\"border-color:rgba(255,80,80,.35)\">RED</button>"
        " <button class=\"btn-ghost\" id=\"bGreen\" style=\"border-color:rgba(80,255,140,.35)\">GREEN</button>"
        " <button class=\"btn-ghost\" id=\"bBlue\"  style=\"border-color:rgba(80,160,255,.35)\">BLUE</button>"
        " <button class=\"btn-ghost\" id=\"bCyan\"  style=\"border-color:rgba(0,240,208,.35)\">CYAN</button>"
        "</div>"
        "<div style=\"display:flex;justify-content:center;gap:.8rem;margin-top:1.0rem;\">"
        " <button id=\"start\">Start</button>"
        " <button class=\"btn-ghost\" id=\"reset\">Reset</button>"
        "</div>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    append_helpers(p, BUF_SIZE);

    strncat(p,
        "<script>"
        "loadBest('Stroop','best');"
        "const stim=document.getElementById('stim');"
        "const tEl=document.getElementById('t');"
        "const okEl=document.getElementById('ok');"
        "const msg=document.getElementById('msg');"
        "const COLORS=["
        " {name:'RED',   css:'#ff5a5a'},"
        " {name:'GREEN', css:'#52ff9a'},"
        " {name:'BLUE',  css:'#4aa0ff'},"
        " {name:'CYAN',  css:'#00f0d0'}"
        "];"
        "let running=false, left=30, timer=0, correct=0, answer='RED';"
        "function pick(){"
        " const word=COLORS[Math.floor(Math.random()*COLORS.length)].name;"
        " const col=COLORS[Math.floor(Math.random()*COLORS.length)];"
        " stim.textContent=word;"
        " stim.style.color=col.css;"
        " answer=col.name;"
        "}"
        "function end(){"
        " running=false;"
        " clearInterval(timer);"
        " msg.textContent=`Done. ${correct} correct.`;"
        " record('Stroop', `${correct} correct`);"
        " setTimeout(()=>loadBest('Stroop','best'),250);"
        "}"
        "function start(){"
        " if(running) return;"
        " running=true; left=30; correct=0;"
        " tEl.textContent=String(left); okEl.textContent='0';"
        " msg.textContent='GO';"
        " pick();"
        " timer=setInterval(()=>{"
        "  left--; tEl.textContent=String(left);"
        "  if(left<=0) end();"
        " },1000);"
        "}"
        "function reset(){"
        " running=false; clearInterval(timer);"
        " left=30; correct=0;"
        " tEl.textContent='30'; okEl.textContent='0';"
        " stim.textContent='READY'; stim.style.color='var(--txt)';"
        " msg.textContent='';"
        "}"
        "function press(choice){"
        " if(!running) return;"
        " if(choice===answer){"
        "  correct++; okEl.textContent=String(correct);"
        "  msg.textContent='✔';"
        " }else{"
        "  msg.textContent=`✖ was ${answer}`;"
        " }"
        " pick();"
        "}"
        "document.getElementById('start').onclick=start;"
        "document.getElementById('reset').onclick=reset;"
        "document.getElementById('bRed').onclick=()=>press('RED');"
        "document.getElementById('bGreen').onclick=()=>press('GREEN');"
        "document.getElementById('bBlue').onclick=()=>press('BLUE');"
        "document.getElementById('bCyan').onclick=()=>press('CYAN');"
        "reset();"
        "</script>",
        BUF_SIZE - strlen(p) - 1
    );

    append_footer(p, BUF_SIZE);
    return p;
}

// Dashboard
static char *build_dashboard_html(void) {
    int total = 0;
    EnterCriticalSection(&metrics_cs);
    total = metric_count;
    LeaveCriticalSection(&metrics_cs);

    char *p = (char*)calloc(1, BUF_SIZE);
    if (!p) return NULL;

    char tmp[64];

    strncat(p,
        "<div class=\"wrap\">"
        "<div class=\"topbar\">"
        "<div class=\"pill\"><span style=\"color:var(--accent);font-weight:1200;\">Cognitive Arena</span>"
        "<span class=\"muted\" id=\"clk\"></span></div>"
        "<div class=\"pill\"><span class=\"muted\">Total results</span>"
        "<span style=\"color:var(--accent);font-weight:1200;\" id=\"total\">",
        BUF_SIZE - strlen(p) - 1
    );

    snprintf(tmp, sizeof(tmp), "%d", total);
    strncat(p, tmp, BUF_SIZE - strlen(p) - 1);

    strncat(p,
        "</span></div></div>"
        "<script>const clk=()=>{document.getElementById('clk').textContent=new Date().toLocaleString();};clk();setInterval(clk,1000);</script>",
        BUF_SIZE - strlen(p) - 1
    );

    strncat(p,
        "<div class=\"kpi\">"
        "<div class=\"glass\"><div class=\"muted\">Reaction best</div><div class=\"v\" id=\"bR\">—</div></div>"
        "<div class=\"glass\"><div class=\"muted\">Typing best</div><div class=\"v\" id=\"bT\">—</div></div>"
        "<div class=\"glass\"><div class=\"muted\">Stroop best</div><div class=\"v\" id=\"bS\">—</div></div>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    strncat(p,
        "<div class=\"glass\" style=\"overflow-x:auto;\">"
        "<h2 style=\"margin-bottom:.5rem;\">Recent Results</h2>"
        "<p class=\"muted\" style=\"margin-bottom:1rem;\">Updates live.</p>"
        "<table><thead><tr><th>Game</th><th>Time</th><th>Result</th></tr></thead>"
        "<tbody id=\"recent\"></tbody></table></div>",
        BUF_SIZE - strlen(p) - 1
    );

    strncat(p,
        "<h2 style=\"text-align:center;margin:2.8rem 0 1.25rem;\">Choose Training Mode</h2>"
        "<div class=\"card-grid\">"
        "<a href=\"/reaction\"><div class=\"glass\"><span class=\"tag\">Reflex</span><h3>Reaction Speed</h3><p class=\"muted\">Best: <span id=\"br2\">—</span></p></div></a>"
        "<a href=\"/velocity\"><div class=\"glass\"><span class=\"tag\">Speed</span><h3>Click Velocity</h3><p class=\"muted\">Best: <span id=\"bv2\">—</span></p></div></a>"
        "<a href=\"/aim\"><div class=\"glass\"><span class=\"tag\">Accuracy</span><h3>Aim Trainer</h3><p class=\"muted\">Best: <span id=\"ba2\">—</span></p></div></a>"
        "<a href=\"/memory\"><div class=\"glass\"><span class=\"tag\">Memory</span><h3>Memory Tiles</h3><p class=\"muted\">Best: <span id=\"bm2\">—</span></p></div></a>"
        "<a href=\"/typing\"><div class=\"glass\"><span class=\"tag\">Typing</span><h3>Typing Burst</h3><p class=\"muted\">Best: <span id=\"bt2\">—</span></p></div></a>"
        "<a href=\"/focus\"><div class=\"glass\"><span class=\"tag\">Attention</span><h3>Stroop Sprint</h3><p class=\"muted\">Best: <span id=\"bs2\">—</span></p></div></a>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    strncat(p,
        "<div class=\"footer-note\">Cognitive Arena - 2026</div>"
        "<script>"
        "function esc(s){return (s||'').replace(/[&<>\"']/g,m=>({\"&\":\"&amp;\",\"<\":\"&lt;\",\">\":\"&gt;\",\"\\\"\":\"&quot;\",\"'\":\"&#39;\"}[m]));}"
        "function decodeBest(v){"
        "  if(!v) return '—';"
        "  try{ if(v.includes('%')||v.includes('+')) return decodeURIComponent(v.replace(/\\+/g,' ')); }catch(e){}"
        "  return v.replace(/%20/g,' ');"
        "}"
        "async function refresh(){"
        " try{"
        "  const r=await fetch('/metrics.json',{cache:'no-store'});"
        "  const j=await r.json();"
        "  document.getElementById('total').textContent=j.total;"
        "  document.getElementById('bR').textContent=decodeBest(j.best.Reaction);"
        "  document.getElementById('bT').textContent=decodeBest(j.best.Typing);"
        "  document.getElementById('bS').textContent=decodeBest(j.best.Stroop);"
        "  document.getElementById('br2').textContent=decodeBest(j.best.Reaction);"
        "  document.getElementById('bv2').textContent=decodeBest(j.best.Velocity);"
        "  document.getElementById('ba2').textContent=decodeBest(j.best.AimTrainer);"
        "  document.getElementById('bm2').textContent=decodeBest(j.best.Memory);"
        "  document.getElementById('bt2').textContent=decodeBest(j.best.Typing);"
        "  document.getElementById('bs2').textContent=decodeBest(j.best.Stroop);"
        "  const tb=document.getElementById('recent'); tb.innerHTML='';"
        "  (j.recent||[]).forEach(m=>{"
        "   const tr=document.createElement('tr');"
        "   tr.innerHTML=`<td>${esc(m.game)}</td><td>${esc(m.time)}</td><td style=\"color:var(--accent);font-weight:1000;\">${esc(decodeBest(m.score))}</td>`;"
        "   tb.appendChild(tr);"
        "  });"
        " }catch(e){}"
        "}"
        "refresh(); setInterval(refresh, 1000);"
        "</script>"
        "</div>",
        BUF_SIZE - strlen(p) - 1
    );

    return p;
}

static char *build_metrics_json(void) {
    char bestR[96], bestV[96], bestA[96], bestM[96], bestT[96], bestS[96];
    best_for_game("Reaction", bestR);
    best_for_game("Velocity", bestV);
    best_for_game("AimTrainer", bestA);
    best_for_game("Memory", bestM);
    best_for_game("Typing", bestT);
    best_for_game("Stroop", bestS);

    Metric recent[10];
    int n = 0;
    int total = 0;

    EnterCriticalSection(&metrics_cs);
    total = metric_count;
    int show = metric_count > 10 ? 10 : metric_count;
    for (int i = metric_count - show; i < metric_count; i++) recent[n++] = metrics[i];
    LeaveCriticalSection(&metrics_cs);

    char *body = (char*)calloc(1, BUF_SIZE);
    if (!body) return http_response("application/json", "{\"error\":1}", 500, "Internal Server Error");

    char *w = body;
    w += snprintf(w, BUF_SIZE - (w - body),
        "{"
        "\"total\":%d,"
        "\"best\":{"
        "\"Reaction\":\"%s\","
        "\"Velocity\":\"%s\","
        "\"AimTrainer\":\"%s\","
        "\"Memory\":\"%s\","
        "\"Typing\":\"%s\","
        "\"Stroop\":\"%s\""
        "},"
        "\"recent\":[",
        total, bestR, bestV, bestA, bestM, bestT, bestS
    );

    for (int i = 0; i < n; i++) {
        char g[64], t[64], s[128];
        snprintf(g, sizeof(g), "%s", recent[i].game);
        snprintf(t, sizeof(t), "%s", recent[i].time_s);
        snprintf(s, sizeof(s), "%s", recent[i].score);
        normalize_score(s, sizeof(s));

        for (char *q = g; *q; q++) if (*q == '\"') *q = '\'';
        for (char *q = t; *q; q++) if (*q == '\"') *q = '\'';
        for (char *q = s; *q; q++) if (*q == '\"') *q = '\'';

        w += snprintf(w, BUF_SIZE - (w - body),
            "%s{\"game\":\"%s\",\"time\":\"%s\",\"score\":\"%s\"}",
            (i ? "," : ""), g, t, s
        );
    }

    snprintf(w, BUF_SIZE - (w - body), "]}");

    char *resp = http_response("application/json; charset=UTF-8", body, 200, "OK");
    free(body);
    return resp;
}

static char *build_best_json(const char *game) {
    char best[96];
    best_for_game(game, best);
    char body[256];
    snprintf(body, sizeof(body), "{\"game\":\"%s\",\"best\":\"%s\"}", game ? game : "", best);
    return http_response("application/json; charset=UTF-8", body, 200, "OK");
}

// Queue
static SOCKET q[QMAX];
static int qh = 0, qt = 0;
static CRITICAL_SECTION q_cs;
static HANDLE q_sem;

static void q_push(SOCKET s) {
    EnterCriticalSection(&q_cs);
    int nt = (qt + 1) % QMAX;
    if (nt != qh) {
        q[qt] = s;
        qt = nt;
        ReleaseSemaphore(q_sem, 1, NULL);
    } else {
        closesocket(s);
    }
    LeaveCriticalSection(&q_cs);
}

static SOCKET q_pop(void) {
    WaitForSingleObject(q_sem, INFINITE);
    EnterCriticalSection(&q_cs);
    SOCKET s = INVALID_SOCKET;
    if (qh != qt) {
        s = q[qh];
        qh = (qh + 1) % QMAX;
    }
    LeaveCriticalSection(&q_cs);
    return s;
}

// Router
static void handle_client(SOCKET s) {
    static char buf[BUF_SIZE];
    int total = 0;

    while (total < (int)sizeof(buf) - 1) {
        int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) { closesocket(s); return; }
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }

    char method[8] = {0}, uri[512] = {0}, ver[16] = {0};
    if (sscanf(buf, "%7s %511s %15s", method, uri, ver) != 3) {
        char *resp = http_response("text/html; charset=UTF-8", "<h1>400</h1>", 400, "Bad Request");
        if (resp) { send_all(s, resp, (int)strlen(resp)); free(resp); }
        closesocket(s);
        return;
    }

    if (icmp(method, "GET") != 0) {
        char *resp = http_response("text/html; charset=UTF-8", "<h1>405</h1>", 405, "Method Not Allowed");
        if (resp) { send_all(s, resp, (int)strlen(resp)); free(resp); }
        closesocket(s);
        return;
    }

    char path[256] = {0};
    char qs[512] = {0};
    const char *qmark = strchr(uri, '?');
    if (qmark) {
        size_t plen = (size_t)(qmark - uri);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, uri, plen);
        path[plen] = 0;
        strncpy(qs, qmark + 1, sizeof(qs) - 1);
    } else {
        strncpy(path, uri, sizeof(path) - 1);
    }

    if (strstr(path, "..") || strlen(path) > 200) {
        char *resp = http_response("text/html; charset=UTF-8", "<h1>404</h1>", 404, "Not Found");
        if (resp) { send_all(s, resp, (int)strlen(resp)); free(resp); }
        closesocket(s);
        return;
    }

    char *resp = NULL;

    if (strcmp(path, "/record") == 0) {
        char game[64], score[128];
        if (!get_qs_param(qs, "game", game, sizeof(game))) strcpy(game, "Unknown");
        if (!get_qs_param(qs, "score", score, sizeof(score))) strcpy(score, "—");
        game[31] = 0;
        score[95] = 0;
        normalize_score(score, sizeof(score));
        record_metric(game, score);
        resp = http_response("text/plain; charset=UTF-8", "OK", 200, "OK");
    }
    else if (strcmp(path, "/metrics.json") == 0) {
        resp = build_metrics_json();
    }
    else if (strcmp(path, "/best.json") == 0) {
        char game[64];
        if (!get_qs_param(qs, "game", game, sizeof(game))) strcpy(game, "Unknown");
        game[31] = 0;
        resp = build_best_json(game);
    }
    else if (strcmp(path, "/") == 0) {
        char *html = build_dashboard_html();
        if (html) { resp = build_page("Dashboard", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else if (strcmp(path, "/reaction") == 0) {
        char *html = build_reaction_html();
        if (html) { resp = build_page("Reaction Speed", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else if (strcmp(path, "/velocity") == 0) {
        char *html = build_velocity_html();
        if (html) { resp = build_page("Click Velocity", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else if (strcmp(path, "/aim") == 0) {
        char *html = build_aim_html();
        if (html) { resp = build_page("Aim Trainer", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else if (strcmp(path, "/memory") == 0) {
        char *html = build_memory_html();
        if (html) { resp = build_page("Memory Tiles", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else if (strcmp(path, "/typing") == 0) {
        char *html = build_typing_html();
        if (html) { resp = build_page("Typing Burst", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else if (strcmp(path, "/focus") == 0 || strcmp(path, "/stroop") == 0) {
        char *html = build_stroop_html();
        if (html) { resp = build_page("Stroop Sprint", html); free(html); }
        else resp = http_response("text/html; charset=UTF-8", "<h1>500</h1>", 500, "Internal Server Error");
    }
    else {
        resp = http_response("text/html; charset=UTF-8", "<h1>404 – Not Found</h1>", 404, "Not Found");
    }

    if (resp) {
        send_all(s, resp, (int)strlen(resp));
        free(resp);
    }
    closesocket(s);
}

static DWORD WINAPI worker_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        SOCKET s = q_pop();
        if (s != INVALID_SOCKET) handle_client(s);
    }
    return 0;
}

// Main
int main(void) {
    WSADATA ws;
    if (WSAStartup(MAKEWORD(2,2), &ws) != 0) return 1;

    InitializeCriticalSection(&metrics_cs);
    InitializeCriticalSection(&q_cs);
    q_sem = CreateSemaphoreA(NULL, 0, QMAX, NULL);

    load_metrics();

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { WSACleanup(); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR ||
        listen(srv, BACKLOG) == SOCKET_ERROR) {
        closesocket(srv);
        WSACleanup();
        return 1;
    }

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    printf("Cognitive Arena running -> http://127.0.0.1:%d\n", PORT);
    printf("Routes: / /reaction /velocity /aim /memory /typing /focus (/stroop)\n");
    printf("Live: /metrics.json\n");

    for (;;) {
        SOCKET cl = accept(srv, NULL, NULL);
        if (cl == INVALID_SOCKET) continue;
        q_push(cl);
    }

    closesocket(srv);
    WSACleanup();
    DeleteCriticalSection(&metrics_cs);
    DeleteCriticalSection(&q_cs);
    return 0;
}
