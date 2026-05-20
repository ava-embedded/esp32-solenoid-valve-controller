#include <Arduino.h>
#include <WiFi.h> 
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// =================================================================================
//  NOTA: SE PUEDEN AGREGAR MÁS IMÁGENES EN LA CARPETA "DATA" Y USARLAS EN EL HTML
//  SE USÓ VSCODE JUNTO CON PLATFORMIO PARA EL DISEÑO DEL PROYECTO, TODOS LOS  
//  AJUSTES ESTÁN EN EL ARCHIVO .INI.
//
//  ADRIAN VALLE ARAUJO - PRACTICANTE DE MANTENIMIENTO ENVASADO
//  RAFAEL PEDRERA SANSORES - ASESOR 
// =================================================================================

// NUNCA CAMBIAR -- DEFINICIÓN DE LAS ELECTROVÁLVULAS PARA LA LÓGICA DEL MICRO
#define NUM_VALVES 5
const int VALVE_PINS[NUM_VALVES]    = {32, 33, 25, 26, 27};
const char* VALVE_NAMES[NUM_VALVES] = {
  "Electroválvula 3","Electroválvula 4","Electroválvula 5",
  "Electroválvula 7","Electroválvula 8"
};

// ----------- RED Y CONTRASEÑA -----------
const char* AP_SSID     = "Control EVs";
const char* AP_PASSWORD = "12345678";

AsyncWebServer server(80);

// -------ESTRUCTURAS DE SECUENCIA ----> esto se usó antes de los ajustes pero puede servir de referencia o para reusar
struct SeqStep { int valveIdx; bool open; unsigned long tMs; };

// Llenado lento — 1000 ms
const SeqStep SEQ_SLOW[] = {
  {0,true,0},{1,true,0},{2,true,0},{3,false,0},{4,true,0},{3,true,1000}
};
const int SEQ_SLOW_LEN = sizeof(SEQ_SLOW)/sizeof(SeqStep);
const unsigned long SEQ_SLOW_TOTAL = 1000;

// Llenado rápido — 500 ms
const SeqStep SEQ_FAST[] = {
  {0,true,0},{1,true,0},{2,true,0},{3,true,0},{4,false,0},{4,true,500}
};
const int SEQ_FAST_LEN = sizeof(SEQ_FAST)/sizeof(SeqStep);
const unsigned long SEQ_FAST_TOTAL = 500;

// Presurización — 700 ms
const SeqStep SEQ_PRES[] = {
  {0,false,0},{1,true,0},{2,false,0},{3,false,0},{4,false,0},
  {0,true,700},{2,true,700},{3,true,700},{4,true,700}
};
const int SEQ_PRES_LEN = sizeof(SEQ_PRES)/sizeof(SeqStep);
const unsigned long SEQ_PRES_TOTAL = 700;

// ----------- MOTOR DE SECUENCIA -----------
enum RunState { RUN_IDLE, RUN_ACTIVE };
volatile RunState  runState    = RUN_IDLE;
const SeqStep*     activeSeq   = nullptr;
int                activeLen   = 0;
unsigned long      activeTotal = 0;
unsigned long      seqStart    = 0;
int                seqStepIdx  = 0;

// ── FASES: combinaciones de EVs (lógica inversa: LOW=abre, HIGH=cierra) ──

// Fase 0: Lavado por descarga  → NEU:LOW, P3:LOW, CO2:LOW, P2:HIGH, P1:LOW
// Fase 1: Presurización        → NEU:LOW, P3:HIGH, CO2:LOW, P2:LOW, P1:LOW
// Fase 2a: Llenado lento       → NEU:HIGH, P3:HIGH, CO2:HIGH, P2:LOW, P1:HIGH
// Fase 2b: Llenado rápido      → NEU:HIGH, P3:HIGH, CO2:HIGH, P2:HIGH, P1:LOW
// Fase 3: Descompresión        → NEU:LOW, P3:LOW, CO2:HIGH, P2:HIGH, P1:HIGH

// 5 estados de pin
struct PhaseConfig {
  bool pins[NUM_VALVES];
};

// Manejé estructura para las fases para poder construir una manera segura de establecer la combinación sin
// necesidad de hacerlo directamente EV por EV
const PhaseConfig PHASES[] = {
  {{true,  true,  true,  false, true }},  // 0: Lavado por descarga
  {{true,  false, true,  true,  true }},  // 1: Presurización
  {{false, false, false, true,  false}},  // 2: Llenado Lento
  {{false, false, false, false, true }},  // 3: Llenado Rápido
  {{true,  true,  false, false, false}},  // 4: Descompresión
};
const int NUM_PHASES = sizeof(PHASES)/sizeof(PhaseConfig);

//-----------  HTML -----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Control de Electroválvulas</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=DM+Sans:ital,wght@0,300;0,400;0,500;0,600;0,700&family=DM+Mono:wght@400;500&display=swap" rel="stylesheet">
  <style>
    :root {
      --gold:#C8952A; --navy:#1B2D5B; --bg:#F5F4F0; --surface:#FFFFFF;
      --border:#E2DDD6; --text:#1a1a1a; --muted:#7a7267;
      --safe:#1a8a4a; --danger:#c0392b;
      --font:'DM Sans',sans-serif; --mono:'DM Mono',monospace;
    }
    *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
    body{background:var(--bg);color:var(--text);font-family:var(--font);
      display:flex;flex-direction:column;align-items:center;padding:28px 20px 56px;min-height:100vh}
    .top-bar{width:100%;max-width:1100px;display:flex;align-items:center;
      justify-content:space-between;margin-bottom:28px}
    .logo-modelo img{height:80px;width:auto}
    .logo-abinbev img{height:58px;width:auto}
    .tab-bar{width:100%;max-width:1100px;display:flex;gap:4px;
      background:var(--surface);border:1px solid var(--border);
      border-radius:10px;padding:5px;margin-bottom:28px;
      box-shadow:0 2px 8px rgba(27,45,91,.06)}
    .tab{flex:1;padding:10px 0;border:none;border-radius:7px;
      font-family:var(--font);font-size:13px;font-weight:600;
      letter-spacing:.6px;text-transform:uppercase;cursor:pointer;
      background:transparent;color:var(--muted);transition:all .18s}
    .tab:hover{color:var(--navy)}
    .tab.active{background:var(--navy);color:#fff;
      box-shadow:0 2px 8px rgba(27,45,91,.25)}
    .view{width:100%;max-width:1100px;display:none}
    .view.active{display:block}
    .ev-grid{display:flex;flex-wrap:wrap;justify-content:center;gap:18px}
    .card{background:var(--surface);border:1px solid var(--border);
      border-radius:12px;padding:20px 18px 18px;
      box-shadow:0 4px 16px rgba(27,45,91,.07)}
    .ev-card{width:196px}
    .card-title{font-size:11px;font-weight:700;letter-spacing:1.8px;
      color:var(--navy);text-transform:uppercase;margin-bottom:12px}
    .status-mini{display:flex;align-items:center;gap:8px;font-size:12px;
      font-weight:500;margin-bottom:12px;padding:7px 10px;
      background:var(--bg);border-radius:7px}
    .dot{width:9px;height:9px;border-radius:50%;background:#ccc;flex-shrink:0;
      transition:background .3s,box-shadow .3s}
    .dot.on {background:var(--safe);  box-shadow:0 0 7px rgba(26,138,74,.5)}
    .dot.off{background:var(--danger);box-shadow:0 0 7px rgba(192,57,43,.4)}
    .dot.seq{background:var(--navy);  box-shadow:0 0 7px rgba(27,45,91,.4);
      animation:blink 1s infinite}
    @keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
    .btn-group{display:flex;flex-direction:column;gap:8px}
    .btn{width:100%;padding:10px 12px;border:none;border-radius:7px;
      font-family:var(--font);font-size:12px;font-weight:600;
      letter-spacing:.6px;text-transform:uppercase;cursor:pointer;transition:all .15s}
    .btn:active{transform:scale(.97)}
    .btn:disabled{opacity:.4;cursor:not-allowed;transform:none}
    .btn-on {background:var(--safe);  color:#fff}
    .btn-off{background:var(--danger);color:#fff}

    /* ── Sección secuencias ── */
    .seq-section-title{font-size:11px;font-weight:700;letter-spacing:1.8px;
      color:var(--navy);text-transform:uppercase;margin-bottom:20px}
    .builder-card{background:var(--surface);border:1px solid var(--border);
      border-radius:12px;box-shadow:0 4px 16px rgba(27,45,91,.07);overflow:hidden}
    .builder-list{padding:0}

    /* ── Bloque de fase (header + dropdown) ── */
    .phase-block{border-bottom:1px solid var(--border)}
    .phase-block:last-child{border-bottom:none}
    .phase-block.active-phase > .phase-row{background:rgba(27,45,91,.04);border-left:3px solid var(--navy)}
    .phase-block.done-phase   > .phase-row{border-left:3px solid var(--safe)}

    /* ── Fila principal de fase ── */
    .phase-row{
      display:grid;
      grid-template-columns:1fr 220px 110px 110px;
      align-items:center;gap:14px;
      padding:15px 22px;
      background:var(--surface);
      cursor:pointer;
      user-select:none;
      transition:background .15s}
    .phase-row:hover{background:rgba(27,45,91,.02)}
    .phase-left{display:flex;flex-direction:column;gap:4px}
    .phase-header-row{display:flex;align-items:center;gap:8px}
    .phase-name{font-size:13px;font-weight:700;color:var(--navy)}
    .phase-chevron{font-size:15px;color:var(--navy);transition:transform .2s;line-height:1;opacity:.6}
    .phase-chevron.open{transform:rotate(180deg)}
    .phase-mod-badge{font-size:9px;font-family:var(--mono);background:rgba(200,149,42,.15);
      color:var(--gold);border:1px solid rgba(200,149,42,.3);padding:1px 6px;border-radius:8px;
      font-weight:600;letter-spacing:.4px;display:none}
    .phase-mod-badge.visible{display:inline-block}
    /* Selector lento/rápido */
    .fill-toggle{display:flex;gap:0;border:1.5px solid var(--border);border-radius:7px;overflow:hidden}
    .fill-btn{flex:1;padding:8px 10px;border:none;background:transparent;
      font-family:var(--font);font-size:11px;font-weight:600;letter-spacing:.5px;
      text-transform:uppercase;cursor:pointer;color:var(--muted);transition:all .15s}
    .fill-btn.active{background:var(--navy);color:#fff}
    .fill-btn:first-child{border-right:1.5px solid var(--border)}

    .ms-input-wrap{display:flex;align-items:center;gap:8px}
    .ms-input{width:100%;padding:8px 10px;border:1.5px solid var(--border);
      border-radius:7px;font-family:var(--mono);font-size:13px;
      color:var(--text);background:var(--bg);outline:none;
      transition:border-color .2s;text-align:right}
    .ms-input:focus{border-color:var(--navy)}
    .ms-label{font-size:11px;color:var(--muted);font-family:var(--mono);white-space:nowrap}
    .btn-test{padding:8px 0;width:100%;border:1.5px solid var(--border);
      border-radius:7px;background:transparent;font-family:var(--font);
      font-size:11px;font-weight:600;letter-spacing:.6px;text-transform:uppercase;
      color:var(--muted);cursor:pointer;transition:all .15s}
    .btn-test:hover{border-color:var(--navy);color:var(--navy)}
    .btn-test:disabled{opacity:.35;cursor:not-allowed}
    .btn-test.testing{border-color:var(--gold);color:var(--gold);animation:blink 1s infinite}
    .phase-status{display:flex;align-items:center;gap:6px;
      font-size:11px;font-weight:500;justify-content:center}

    /* ── Dropdown de EVs ── */
    .phase-dropdown{
      display:none;
      padding:12px 22px 16px 22px;
      background:#faf9f7;
      border-top:1px dashed var(--border)}
    .phase-dropdown.open{display:block}
    .dropdown-title{font-size:10px;font-weight:700;letter-spacing:1.4px;
      text-transform:uppercase;color:var(--muted);margin-bottom:10px}
    .ev-toggle-list{display:flex;flex-direction:column;gap:6px}
    .ev-toggle-row{display:flex;align-items:center;justify-content:space-between;
      padding:8px 12px;background:var(--surface);border:1px solid var(--border);
      border-radius:8px;transition:border-color .15s}
    .ev-toggle-row:hover{border-color:var(--navy)}
    .ev-toggle-name{font-size:12px;font-weight:600;color:var(--navy)}
    .ev-toggle-switch{display:flex;gap:0;border:1.5px solid var(--border);
      border-radius:6px;overflow:hidden}
    .ev-toggle-btn{padding:5px 14px;border:none;background:transparent;
      font-family:var(--font);font-size:11px;font-weight:600;letter-spacing:.4px;
      text-transform:uppercase;cursor:pointer;color:var(--muted);transition:all .12s}
    .ev-toggle-btn:first-child{border-right:1.5px solid var(--border)}
    .ev-toggle-btn.sel-open {background:var(--safe); color:#fff;border-color:var(--safe)}
    .ev-toggle-btn.sel-close{background:var(--danger);color:#fff;border-color:var(--danger)}
    .dropdown-reset{margin-top:10px;font-size:10px;font-family:var(--mono);
      color:var(--muted);cursor:pointer;background:none;border:none;
      text-decoration:underline;padding:0;transition:color .15s}
    .dropdown-reset:hover{color:var(--navy)}

    .builder-footer{padding:16px 22px;border-top:1px solid var(--border);
      display:flex;align-items:center;justify-content:space-between;gap:16px}
    .builder-total{font-size:11px;font-family:var(--mono);color:var(--muted)}
    .builder-total span{color:var(--navy);font-weight:600}
    .btn-exec{padding:12px 32px;background:var(--navy);color:#fff;
      border:none;border-radius:8px;font-family:var(--font);font-size:13px;
      font-weight:700;letter-spacing:.8px;text-transform:uppercase;
      cursor:pointer;transition:all .15s;
      box-shadow:0 2px 12px rgba(27,45,91,.3)}
    .btn-exec:hover{background:#263d7a}
    .btn-exec:disabled{opacity:.4;cursor:not-allowed}
    .btn-exec.running{background:var(--gold);box-shadow:0 2px 12px rgba(200,149,42,.35)}
    .footer{margin-top:40px;font-size:11px;color:var(--muted);
      font-family:var(--mono);border-top:1px solid var(--border);
      padding-top:18px;width:100%;max-width:1100px;text-align:center}
    .footer span{color:var(--gold);font-weight:600}
  </style>
</head>
<body>

<div class="top-bar">
  <div class="logo-modelo"><img src="/logo_grupo_modelo.png" alt="Grupo Modelo"></div>
  <div class="logo-abinbev"><img src="/logo_abinbev.png" alt="AB InBev"></div>
</div>

<div class="tab-bar">
  <button class="tab active" onclick="switchTab('general')">General</button>
  <button class="tab" onclick="switchTab('secuencias')">Secuencias</button>
</div>

<div class="view active" id="view-general">
  <div class="ev-grid" id="ev-grid"></div>
</div>

<div class="view" id="view-secuencias">
  <p class="seq-section-title">Secuencia de Llenado</p>
  <div class="builder-card">
    <div class="builder-list" id="builder-list"></div>
    <div class="builder-footer">
      <div class="builder-total">Total estimado: <span id="builder-total">0</span> ms</div>
      <div style="display:flex;align-items:center;gap:12px">
        <div style="display:flex;align-items:center;gap:8px">
          <label style="font-size:11px;font-family:var(--mono);color:var(--muted);white-space:nowrap">Ciclos</label>
          <input type="number" id="cycle-count" min="1" max="99" value="1"
            style="width:64px;padding:8px 10px;border:1.5px solid var(--border);border-radius:7px;
                  font-family:var(--mono);font-size:13px;color:var(--text);background:var(--bg);
                  outline:none;text-align:center;transition:border-color .2s"
            onfocus="this.style.borderColor='var(--navy)'"
            onblur="this.style.borderColor='var(--border)'">
          <span style="font-size:11px;font-family:var(--mono);color:var(--muted)" id="cycle-status"></span>
        </div>
        <button class="btn-exec" id="btn-exec" onclick="execCustom()">&#9654; Ejecutar Secuencia</button>
      </div>
    </div>
  </div>
</div>

<div class="footer">Adrian Valle · <span>Packaging</span> · Cervecería Yucateca</div>

<script>
const VALVES = [
  {id:0, name:"NEUMÁTICA"},
  {id:1, name:"PISTON 3"},
  {id:2, name:"CO2"},
  {id:3, name:"PISTON 2"},
  {id:4, name:"PISTON 1"}
];

// true = LOW (abierta), false = HIGH (cerrada) — lógica inversa
const PHASES = [
  {
    id:0, name:"Lavado por Descarga",
    defaultPins:[true, true, true, false, true],
    hasFillToggle:false
  },
  {
    id:1, name:"Presurización",
    defaultPins:[true, false, true, true, true],
    hasFillToggle:false
  },
  {
    id:2, name:"Llenado",
    defaultPinsSlow:[false, false, false, true,  false],
    defaultPinsFast:[false, false, false, false, true],
    hasFillToggle:true,
    fillMode:'slow'
  },
  {
    id:3, name:"Descompresión",
    defaultPins:[true, true, false, false, false],
    hasFillToggle:false
  }
];

// Estado mutable de pines por fase (inicializado desde defaults)
// customPins[pos] = [bool×5]
const customPins = PHASES.map(ph => {
  if (ph.hasFillToggle) return [...ph.defaultPinsSlow];
  return [...ph.defaultPins];
});

function getDefaultPins(pos) {
  const ph = PHASES[pos];
  if (ph.hasFillToggle) return ph.fillMode === 'slow' ? [...ph.defaultPinsSlow] : [...ph.defaultPinsFast];
  return [...ph.defaultPins];
}

function isPinsModified(pos) {
  const def = getDefaultPins(pos);
  return customPins[pos].some((v, i) => v !== def[i]);
}


// ── Tab switch ────────────────────────────────────────────────────────────
function switchTab(tab) {
  document.querySelectorAll('.tab').forEach((t,i) => {
    t.classList.toggle('active', ['general','secuencias'][i] === tab);
  });
  document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
  document.getElementById('view-' + tab).classList.add('active');
}

// ── Tarjetas General ──────────────────────────────────────────────────────
function createCards() {
  const grid = document.getElementById('ev-grid');
  VALVES.forEach(v => {
    const card = document.createElement('div');
    card.className = 'card ev-card';
    card.innerHTML =
      '<div class="card-title">'+v.name+'</div>'+
      '<div class="status-mini"><div class="dot" id="dot-'+v.id+'"></div><span id="txt-'+v.id+'">Listo</span></div>'+
      '<div class="btn-group">'+
      '<button class="btn btn-on"  onclick="sendManual(\'on\','+v.id+')">Encender</button>'+
      '<button class="btn btn-off" onclick="sendManual(\'off\','+v.id+')">Apagar</button>'+
      '</div>';
    grid.appendChild(card);
  });
}

function sendManual(action, idx) {
  fetch('/'+action+'?v='+idx).then(() => {
    document.getElementById('dot-'+idx).className = 'dot '+(action==='on'?'on':'off');
    document.getElementById('txt-'+idx).textContent = action==='on'?'ENCENDIDA':'APAGADA';
  }).catch(() => { document.getElementById('txt-'+idx).textContent='Error'; });
}

// ── Construcción de filas de fase ─────────────────────────────────────────
function buildRows() {
  const list = document.getElementById('builder-list');
  list.innerHTML = '';
  PHASES.forEach((ph, pos) => {

    // ── Bloque contenedor (header + dropdown)
    const block = document.createElement('div');
    block.className = 'phase-block';
    block.id = 'phblock-' + pos;

    // ── Fila principal
    const row = document.createElement('div');
    row.className = 'phase-row';
    row.id = 'phrow-' + pos;

    // Columna izquierda
    let leftHtml = '<div class="phase-left">' +
      '<div class="phase-header-row">' +
        '<div class="phase-name">' + ph.name + '</div>' +
        '<span class="phase-chevron" id="chev-'+pos+'">▾</span>' +
        '<span class="phase-mod-badge" id="modbadge-'+pos+'">MODIFICADA</span>' +
      '</div>';

    if (ph.hasFillToggle) {
      leftHtml +=
        '<div class="fill-toggle" style="margin-top:6px" onclick="event.stopPropagation()">' +
          '<button class="fill-btn '+(ph.fillMode==='slow'?'active':'')+'" ' +
            'id="fbtn-slow-'+pos+'" onclick="setFillMode('+pos+',\'slow\')">Lento</button>' +
          '<button class="fill-btn '+(ph.fillMode==='fast'?'active':'')+'" ' +
            'id="fbtn-fast-'+pos+'" onclick="setFillMode('+pos+',\'fast\')">Rápido</button>' +
        '</div>';
    }
    leftHtml += '</div>';

    row.innerHTML =
      leftHtml +
      '<div class="ms-input-wrap" onclick="event.stopPropagation()">' +
        '<input class="ms-input" type="number" min="10" max="30000" value="500" id="ms-'+pos+'" ' +
          'oninput="updateTotal()" placeholder="500">' +
        '<span class="ms-label">ms</span>' +
      '</div>' +
      '<button class="btn-test" id="test-'+pos+'" onclick="event.stopPropagation();testPhase('+pos+')">Probar</button>' +
      '<div class="phase-status"><div class="dot" id="pdot-'+pos+'"></div><span id="ptxt-'+pos+'">—</span></div>';

    // Click en la fila abre/cierra el dropdown
    row.addEventListener('click', () => toggleDropdown(pos));

    // ── Dropdown de EVs
    const dd = document.createElement('div');
    dd.className = 'phase-dropdown';
    dd.id = 'phdd-' + pos;
    dd.innerHTML = buildDropdownHTML(pos);

    block.appendChild(row);
    block.appendChild(dd);
    list.appendChild(block);
  });
  updateTotal();
}

function buildDropdownHTML(pos) {
  let html = '<div class="dropdown-title">Configuración de Electroválvulas</div>' +
    '<div class="ev-toggle-list">';
  VALVES.forEach((v, vi) => {
    const isOpen = customPins[pos][vi];
    html +=
      '<div class="ev-toggle-row">' +
        '<span class="ev-toggle-name">' + v.name + '</span>' +
        '<div class="ev-toggle-switch">' +
          '<button class="ev-toggle-btn '+(isOpen?'sel-open':'')+'" ' +
            'id="evbtn-open-'+pos+'-'+vi+'" ' +
            'onclick="setPinState('+pos+','+vi+',true)">ABIERTA</button>' +
          '<button class="ev-toggle-btn '+(isOpen?'':'sel-close')+'" ' +
            'id="evbtn-close-'+pos+'-'+vi+'" ' +
            'onclick="setPinState('+pos+','+vi+',false)">CERRADA</button>' +
        '</div>' +
      '</div>';
  });
  html += '</div>' +
    '<button class="dropdown-reset" onclick="resetPhase('+pos+')">↺ Restaurar valores por defecto</button>';
  return html;
}

function toggleDropdown(pos) {
  const dd   = document.getElementById('phdd-'+pos);
  const chev = document.getElementById('chev-'+pos);
  const isOpen = dd.classList.toggle('open');
  chev.classList.toggle('open', isOpen);
}

function setPinState(pos, vi, isOpen) {
  customPins[pos][vi] = isOpen;
  // Actualizar botones
  document.getElementById('evbtn-open-'+pos+'-'+vi).className  = 'ev-toggle-btn '+(isOpen?'sel-open':'');
  document.getElementById('evbtn-close-'+pos+'-'+vi).className = 'ev-toggle-btn '+(isOpen?'':'sel-close');
  // Mostrar badge "MODIFICADA" si difiere del default
  document.getElementById('modbadge-'+pos).classList.toggle('visible', isPinsModified(pos));
}

function resetPhase(pos) {
  const def = getDefaultPins(pos);
  def.forEach((v, vi) => { customPins[pos][vi] = v; });
  // Reconstruir dropdown
  document.getElementById('phdd-'+pos).innerHTML = buildDropdownHTML(pos);
  document.getElementById('modbadge-'+pos).classList.remove('visible');
}

function setFillMode(pos, mode) {
  PHASES[pos].fillMode = mode;
  document.getElementById('fbtn-slow-'+pos).classList.toggle('active', mode==='slow');
  document.getElementById('fbtn-fast-'+pos).classList.toggle('active', mode==='fast');
  // Siempre aplica el default del nuevo modo al cambiar lento/rápido
  const def = getDefaultPins(pos);
  def.forEach((v, vi) => { customPins[pos][vi] = v; });
  document.getElementById('phdd-'+pos).innerHTML = buildDropdownHTML(pos);
  document.getElementById('modbadge-'+pos).classList.remove('visible');
}

function updateTotal() {
  let total = 0;
  PHASES.forEach((_, pos) => {
    const el = document.getElementById('ms-'+pos);
    if (el) total += parseInt(el.value) || 0;
  });
  document.getElementById('builder-total').textContent = total;
}

// Construye la URL para ejecutar una fase con los pines actuales (custom o default)
function buildPhaseUrl(pos, ms) {
  const pins = customPins[pos];
  let url = '/runphasecustom?t=' + ms;
  pins.forEach((v, i) => { url += '&v' + i + '=' + (v ? 1 : 0); });
  return url;
}

// ── Probar fase individual ────────────────────────────────────────────────
let testTimer = null;
let customBusy = false;

function testPhase(pos) {
  if (customBusy) return;
  const ms  = parseInt(document.getElementById('ms-'+pos).value) || 500;
  const btn = document.getElementById('test-'+pos);
  const dot = document.getElementById('pdot-'+pos);
  const txt = document.getElementById('ptxt-'+pos);
  btn.disabled = true; btn.classList.add('testing');
  dot.className = 'dot seq'; txt.textContent = 'ON';
  fetch(buildPhaseUrl(pos, ms)).catch(()=>{});
  clearTimeout(testTimer);
  testTimer = setTimeout(() => {
    dot.className = 'dot off'; txt.textContent = 'OFF';
    btn.disabled = false; btn.classList.remove('testing');
    setTimeout(() => { dot.className = 'dot'; txt.textContent = '—'; }, 1000);
  }, ms + 150);
}

// ── Ejecutar secuencia completa ───────────────────────────────────────────
function execCustom() {
  if (customBusy) return;
  const totalCycles = Math.max(1, parseInt(document.getElementById('cycle-count').value) || 1);
  customBusy = true;
  const btn = document.getElementById('btn-exec');
  const cycleStatus = document.getElementById('cycle-status');
  btn.disabled = true; btn.classList.add('running'); btn.textContent = 'Ejecutando...';

  const steps = PHASES.map((ph, pos) => ({
    pos,
    ms: Math.max(10, parseInt(document.getElementById('ms-'+pos).value) || 500),
    url: buildPhaseUrl(pos, Math.max(10, parseInt(document.getElementById('ms-'+pos).value) || 500))
  }));
  const cycleMs = steps.reduce((a, s) => a + s.ms, 0);

  function runCycle(cycleNum) {
    if (cycleNum > totalCycles) {
      customBusy = false;
      btn.disabled = false; btn.classList.remove('running'); btn.textContent = '\u25BA Ejecutar Secuencia';
      cycleStatus.textContent = '';
      PHASES.forEach((_, pos) => {
        const dot = document.getElementById('pdot-'+pos);
        const txt = document.getElementById('ptxt-'+pos);
        const blk = document.getElementById('phblock-'+pos);
        blk.classList.remove('active-phase','done-phase');
        if (dot) { setTimeout(() => { dot.className = 'dot'; txt.textContent = '—'; }, 600); }
      });
      // Estado de reposo: NEU abierta, resto cerradas
      fetch('/alloff').catch(()=>{});
      return;
    }
    cycleStatus.textContent = cycleNum + '/' + totalCycles;

    let cursor = 0;
    steps.forEach((s) => {
      const openAt  = cursor;
      const closeAt = cursor + s.ms;
      cursor += s.ms;

      setTimeout(() => {
        fetch(s.url).catch(()=>{});
        const dot = document.getElementById('pdot-' + s.pos);
        const txt = document.getElementById('ptxt-' + s.pos);
        const blk = document.getElementById('phblock-' + s.pos);
        if (dot) { dot.className = 'dot seq'; txt.textContent = 'ON'; }
        document.querySelectorAll('.phase-block').forEach(b => b.classList.remove('active-phase'));
        if (blk) blk.classList.add('active-phase');
      }, openAt);

      setTimeout(() => {
        const dot = document.getElementById('pdot-' + s.pos);
        const txt = document.getElementById('ptxt-' + s.pos);
        const blk = document.getElementById('phblock-' + s.pos);
        if (dot) { dot.className = 'dot off'; txt.textContent = 'OFF'; }
        if (blk) { blk.classList.remove('active-phase'); blk.classList.add('done-phase'); }
      }, closeAt);
    });

    setTimeout(() => runCycle(cycleNum + 1), cycleMs + 150);
  }

  runCycle(1);
}

createCards();
buildRows();
</script>
</body>
</html>
)rawliteral";

// ----------- BASES - LECTURA DE COMBINACIONES -----------
int getValveIdx(AsyncWebServerRequest* req, const char* param = "v") {
  if (req->hasParam(param))
    return constrain(req->getParam(param)->value().toInt(), 0, NUM_VALVES-1);
  return 0;
}

// reposo seguro: neumática = LOW (abierta), todas las demás = HIGH (cerradas)
void stopAll() {
  digitalWrite(VALVE_PINS[0], LOW);   // NEUMÁTICA → LOW (abierta)
  for (int i = 1; i < NUM_VALVES; i++) digitalWrite(VALVE_PINS[i], HIGH);
  Serial.println("[STOP] NEU=LOW(abierta) | P3,CO2,P2,P1=HIGH(cerradas)");
}

// combinación de una fase a los pines 
// 0=Lavado, 1=Presurización, 2=Llenado Lento, 3=Llenado Rápido, 4=Descompresión
void applyPhase(int phaseId) {
  if (phaseId < 0 || phaseId >= NUM_PHASES) return;
  for (int i = 0; i < NUM_VALVES; i++) {
    // PHASES[phaseId].pins[i]: true=LOW(abre), false=HIGH(cierra)
    digitalWrite(VALVE_PINS[i], PHASES[phaseId].pins[i] ? LOW : HIGH);
  }
  Serial.printf("[FASE %d] Aplicada\n", phaseId);
}

// base -> fase temporizada
volatile bool      phaseActive = false;
unsigned long      phaseEnd    = 0;

//  ----------- SETUP -----------
void setup() {
  Serial.begin(115200);
  Serial.println("\n[INICIO] Control 5 EVs - Fases de Llenado");

  if (!LittleFS.begin(true)) Serial.println("[WARN] LittleFS no montado — logos no disponibles");

  for (int i = 0; i < NUM_VALVES; i++) pinMode(VALVE_PINS[i], OUTPUT);
  stopAll(); // Estado inicial: neumática abierta para que no haya paso de cerveza, resto cerradas

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[WiFi] AP listo. IP: ");
  Serial.println(WiFi.softAPIP());

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.serveStatic("/logo_grupo_modelo.png", LittleFS, "/logo_grupo_modelo.png");
  server.serveStatic("/logo_abinbev.png",      LittleFS, "/logo_abinbev.png");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", INDEX_HTML);
  });

  // control manual individual (tab General)
  server.on("/on", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (runState != RUN_IDLE) { req->send(200,"text/plain","BUSY"); return; }
    int idx = getValveIdx(req);
    digitalWrite(VALVE_PINS[idx], LOW); // LOW = abre
    Serial.printf("[EV] %s ENCENDIDA (LOW)\n", VALVE_NAMES[idx]);
    req->send(200,"text/plain","OK");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (runState != RUN_IDLE) { req->send(200,"text/plain","BUSY"); return; }
    int idx = getValveIdx(req);
    digitalWrite(VALVE_PINS[idx], HIGH); // HIGH = cierra
    Serial.printf("[EV] %s APAGADA (HIGH)\n", VALVE_NAMES[idx]);
    req->send(200,"text/plain","OK");
  });

  // Apagar todo (cierra todas las EVs)
  server.on("/alloff", HTTP_GET, [](AsyncWebServerRequest* req) {
    stopAll();
    phaseActive = false;
    Serial.println("[CTRL] Todas las EVs cerradas");
    req->send(200,"text/plain","OK");
  });

  // fase llenado por tiempo
  // controla el tiempo y llama /alloff al terminar
  // también cierra automáticamente cuando vence tms
  server.on("/runphase", HTTP_GET, [](AsyncWebServerRequest* req) {
    int pid = 0;
    unsigned long tMs = 500;
    if (req->hasParam("p")) pid  = constrain(req->getParam("p")->value().toInt(), 0, NUM_PHASES-1);
    if (req->hasParam("t")) tMs  = (unsigned long)req->getParam("t")->value().toInt();
    applyPhase(pid);
    phaseActive = true;
    phaseEnd    = millis() + tMs;
    Serial.printf("[FASE] %d iniciada por %lu ms\n", pid, tMs);
    req->send(200,"text/plain","OK");
  });

  // fase con combinaciones personalizadaas
  server.on("/runphasecustom", HTTP_GET, [](AsyncWebServerRequest* req) {
    unsigned long tMs = 500;
    if (req->hasParam("t")) tMs = (unsigned long)req->getParam("t")->value().toInt();
    char pname[3];
    for (int i = 0; i < NUM_VALVES; i++) {
      snprintf(pname, sizeof(pname), "v%d", i);
      if (req->hasParam(pname)) {
        int val = req->getParam(pname)->value().toInt();
        digitalWrite(VALVE_PINS[i], val ? LOW : HIGH);
      }
    }
    phaseActive = true;
    phaseEnd    = millis() + tMs;
    Serial.printf("[FASE CUSTOM] iniciada por %lu ms\n", tMs);
    req->send(200,"text/plain","OK");
  });
  server.on("/runseq", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (runState != RUN_IDLE) { req->send(200,"text/plain","BUSY"); return; }
    int s = 0;
    if (req->hasParam("s")) s = constrain(req->getParam("s")->value().toInt(), 0, 2);
    switch(s) {
      case 0: activeSeq=SEQ_SLOW; activeLen=SEQ_SLOW_LEN; activeTotal=SEQ_SLOW_TOTAL; break;
      case 1: activeSeq=SEQ_FAST; activeLen=SEQ_FAST_LEN; activeTotal=SEQ_FAST_TOTAL; break;
      case 2: activeSeq=SEQ_PRES; activeLen=SEQ_PRES_LEN; activeTotal=SEQ_PRES_TOTAL; break;
    }
    stopAll(); seqStart=millis(); seqStepIdx=0; runState=RUN_ACTIVE;
    Serial.printf("[SEQ] Secuencia heredada %d iniciada\n", s);
    req->send(200,"text/plain","OK");
  });

  server.begin();
  Serial.println("[HTTP] Servidor HTTP listo.");
}

// ----------- LOOP -----------
void loop() {
  // cierra todo cuando vence el tiempo
  if (phaseActive && millis() >= phaseEnd) {
    stopAll();
    phaseActive = false;
    Serial.println("[FASE] Tiempo vencido — EVs cerradas");
  }

  // lee las secuencias establecidas y una vez terminado cierra todas
  if (runState != RUN_ACTIVE) return;
  unsigned long elapsed = millis() - seqStart;
  while (seqStepIdx < activeLen && elapsed >= activeSeq[seqStepIdx].tMs) {
    const SeqStep& s = activeSeq[seqStepIdx];
    digitalWrite(VALVE_PINS[s.valveIdx], s.open ? LOW : HIGH);
    Serial.printf("[SEQ] t=%lums · %s · %s\n", elapsed, VALVE_NAMES[s.valveIdx], s.open?"ABRE":"CIERRA");
    seqStepIdx++;
  }
  if (elapsed >= activeTotal) {
    stopAll(); runState=RUN_IDLE; activeSeq=nullptr; seqStepIdx=0;
    Serial.println("[SEQ] Completada — todas las EVs cerradas");
  }
}