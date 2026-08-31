// ─── דף ווב ───────────────────────────────────────────────────
static const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="he" dir="rtl">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>מקליט קול</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:Arial,sans-serif;background:#0f0f1a;color:#eee;
         display:flex;flex-direction:column;align-items:center;
         justify-content:center;min-height:100vh;gap:20px;padding:20px}
    h2{font-size:1.4em;letter-spacing:1px}

    /* slot selection */
    .slots{display:flex;gap:12px}
    .slot{width:80px;height:80px;border-radius:12px;border:2px solid #444;
          background:#1a1a2e;font-size:1.1em;font-weight:bold;cursor:pointer;
          display:flex;flex-direction:column;align-items:center;
          justify-content:center;gap:4px;transition:all 0.15s;color:#eee}
    .slot.active{border-color:#00e676;background:#003320}
    .slot.has-rec{border-color:#2196f3}
    .slot .dot{width:10px;height:10px;border-radius:50%;background:#444}
    .slot.has-rec .dot{background:#2196f3}
    .slot.active .dot{background:#00e676}

    /* action buttons */
    .actions{display:flex;gap:14px}
    button{padding:12px 26px;border:none;border-radius:10px;font-size:1em;
           font-weight:bold;cursor:pointer;transition:opacity 0.15s}
    button:active{opacity:0.65}
    button:disabled{opacity:0.3;cursor:default}
    #btnRec  {background:#f44336;color:#fff}
    #btnStop {background:#ff9800;color:#000}
    #btnPlay {background:#00c853;color:#000}

    /* level meter */
    #meter{width:520px;background:#222;border-radius:10px;
           height:44px;overflow:hidden;border:1px solid #333}
    #bar{height:100%;width:0%;border-radius:10px;
         background:linear-gradient(90deg,#00e676 0%,#ffeb3b 65%,#f44336 100%);
         transition:width 0.1s ease}

    /* sliders */
    .sliders{display:flex;gap:40px}
    .sl-box{display:flex;flex-direction:column;align-items:center;gap:8px}
    .sl-box label{font-size:0.85em;color:#aaa}
    .sl-box .val{font-size:1.6em;font-weight:bold;color:#00e676}
    input[type=range]{-webkit-appearance:none;width:180px;height:7px;
                      background:#333;border-radius:4px;outline:none}
    input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;
      width:22px;height:22px;border-radius:50%;background:#00e676;cursor:pointer}
    #micSlider::-webkit-slider-thumb{background:#2196f3}

    #status{font-size:0.85em;color:#777;min-height:1.2em;text-align:center}
    .rec-dot{display:inline-block;width:10px;height:10px;border-radius:50%;
             background:#f44336;margin-left:6px;animation:blink 0.7s infinite}
    @keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
  </style>
</head>
<body>
  <h2>🎙️ מקליט קול — LittleFS</h2>

  <div class="slots">
    <div class="slot active" id="slot1" onclick="selectSlot(1)">
      <div class="dot"></div><span>הקלטה 1</span>
    </div>
    <div class="slot" id="slot2" onclick="selectSlot(2)">
      <div class="dot"></div><span>הקלטה 2</span>
    </div>
    <div class="slot" id="slot3" onclick="selectSlot(3)">
      <div class="dot"></div><span>הקלטה 3</span>
    </div>
  </div>

  <div id="meter"><div id="bar"></div></div>

  <div class="actions">
    <button id="btnRec"  onclick="cmd('record')">⏺ הקלט</button>
    <button id="btnStop" onclick="cmd('stop')" disabled>⏹ עצור</button>
    <button id="btnPlay" onclick="cmd('play')">▶ השמע</button>
  </div>

  <div class="sliders">
    <div class="sl-box">
      <label>🎤 עוצמת מיקרופון</label>
      <div class="val" id="micVal">2</div>
      <input type="range" id="micSlider" min="1" max="10" value="2"
             oninput="setGain('mic',this.value)">
    </div>
    <div class="sl-box">
      <label>🔊 עוצמת השמעה</label>
      <div class="val" id="spkVal">4</div>
      <input type="range" id="spkSlider" min="1" max="10" value="4"
             oninput="setGain('spk',this.value)">
    </div>
  </div>

  <div id="status">מוכן</div>

<script>
let currentSlot = 1;
let hasRec = [false, false, false];
let updates = 0;

function selectSlot(n) {
  currentSlot = n;
  document.querySelectorAll('.slot').forEach((el,i) => {
    el.classList.toggle('active', i+1 === n);
  });
}

async function cmd(action) {
  await fetch('/cmd?a=' + action + '&slot=' + currentSlot);
}

async function setGain(who, val) {
  document.getElementById(who + 'Val').textContent = val;
  await fetch('/gain?who=' + who + '&v=' + val);
}

async function poll() {
  try {
    const d = await (await fetch('/data')).json();
    document.getElementById('bar').style.width = d.level + '%';

    // update buttons
    const rec  = d.state === 'recording';
    const play = d.state === 'playing';
    document.getElementById('btnRec').disabled  = rec || play;
    document.getElementById('btnStop').disabled = !rec && !play;
    document.getElementById('btnPlay').disabled = rec || play;

    // update slots — has a recording?
    d.slots.forEach((has, i) => {
      document.getElementById('slot'+(i+1)).classList.toggle('has-rec', has);
    });

    // status
    let st = d.status;
    if (rec) st = '<span class="rec-dot"></span> ' + st;
    document.getElementById('status').innerHTML = st;

  } catch(e) {
    document.getElementById('status').textContent = 'שגיאת חיבור';
  }
}

setInterval(poll, 150);
poll();
</script>
</body>
</html>
)HTML";