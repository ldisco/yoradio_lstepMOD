var hostname = window.location.hostname;
var modesd = false;
var bigplaylist = false;
const query = window.location.search;
const params = new URLSearchParams(query);
const yoTitle = 'YURadio';
let audiopreview=null;
if(params.size>0){
  if(params.has('host')) hostname=params.get('host');
}
var websocket;
var wserrcnt = 0;
var wstimeout, pongtimeout;
// [FIX][SD+WEB] Таймер клиентского heartbeat для мобильных браузеров.

var wsPingTimer = null;

var shellReady = false;
var shellPathname = '';
var clickUiAttached = false;
function markShellReady(pathname){
  shellReady = true;
  shellPathname = pathname || '';
}
var currentItem = 0;
var sdIndexingActive = false; // [FIX] Флаг индексации SD — блокирует обновление nameset/meta
var trackFactsEnabled = false; // [FIX] Глобальный флаг — включены ли TrackFacts (обновляется из WS)
var sleepTimerState = { m: 0, left: 0, active: 0, alloff: 0, supported: 0 };
var playlistmod = new Date().getTime();
var lastNamesetText = '';
// Троттлинг обновлений meta/nameset по отдельности: не чаще раз в 400 мс, чтобы плейлист не дёргался
var lastMetaUpdate = 0, lastNamesetUpdate = 0;
var META_NAMESET_THROTTLE_MS = 400;
// WebSocket send queue to avoid flooding the server
var wsSendQueue = [];
var wsSendTimer = null;
var WS_SEND_INTERVAL = 80; // ms between sends


var settingsActReceived = false;
var settingsActRetryCount = 0;
var settingsActRetryTimer = null;
function scheduleSettingsActRetry(){
  clearTimeout(settingsActRetryTimer);
  settingsActRetryTimer = setTimeout(function(){
    if(window.location.pathname!='/settings.html') return;
    if(settingsActReceived) return;
    if(settingsActRetryCount >= 3) return;
    settingsActRetryCount++;
    sendWS('getactive=1');
    scheduleSettingsActRetry();
  }, 900);
}
function enqueueWS(msg){
  try{ msg = String(msg); }catch(e){ msg = ''+msg; }
  wsSendQueue.push(msg);
  if(!wsSendTimer) wsSendTimer = setTimeout(processWSSendQueue, WS_SEND_INTERVAL);
}
function processWSSendQueue(){
  if(!wsSendQueue.length){ clearTimeout(wsSendTimer); wsSendTimer = null; return; }
  if(websocket && websocket.readyState===WebSocket.OPEN){
    try{ websocket.send(wsSendQueue.shift()); }catch(e){ /* ignore */ }
  }
  wsSendTimer = setTimeout(processWSSendQueue, WS_SEND_INTERVAL);
}
function sendWS(msg, immediate=false){
  if(immediate && websocket && websocket.readyState===WebSocket.OPEN){
    try{ websocket.send(String(msg)); }catch(e){}
    return;
  }
  enqueueWS(msg);
}

// === COVER ART LOADER ===
(function(){var s=document.createElement('script');s.src='/cover.js?_='+Date.now();document.head.appendChild(s);})();
// === END COVER ART ===

// === TRACK FACTS LOADER ===
(function(){var s=document.createElement('script');s.src='/facts.js?_='+Date.now();document.head.appendChild(s);})();
// === END TRACK FACTS ===

window.addEventListener('load', onLoad);

function loadCSS(href){ const link = document.createElement("link"); link.rel = "stylesheet"; link.href = href; document.head.appendChild(link); }
function loadJS(src, callback){ const script = document.createElement("script"); script.src = src; script.type = "text/javascript"; script.async = true; script.onload = callback; document.head.appendChild(script); }
// Подгрузка фрагментов UI: при занятости ESP (SSL/аудио) fetch может «висеть» — снимаем спиннер по таймауту.
function fetchShellHtml(url, timeoutMs){
  timeoutMs = (typeof timeoutMs === 'number' && timeoutMs > 0) ? timeoutMs : 20000;
  return Promise.race([
    fetch(url).then(function(r){ return r.text(); }),
    new Promise(function(_, reject){
      setTimeout(function(){ reject(new Error('shell fetch timeout')); }, timeoutMs);
    })
  ]);
}

// Возвращает яркий цвет полосы буфера по порогам заполнения.
// 0-15%   -> ярко-красный,
// 15-50%  -> ярко-жёлтый,
// 50-100% -> ярко-зелёный.
function getHeapColorByPercent(percent){

  var p = Number(percent);
  if (!Number.isFinite(p)) p = 0;

  // Жёстко ограничиваем диапазон 0..100.
  if (p < 0) p = 0;
  if (p > 100) p = 100;

  // Ярко-красный сегмент.
  if (p <= 15) return '#ff3f2f';
  // Ярко-жёлтый сегмент.
  if (p <= 50) return '#f2cb2f';
  // Ярко-зелёный сегмент.
  return '#39d964';
}

function initWebSocket() {
  clearTimeout(wstimeout);

  stopWsHeartbeat();
  console.log('Trying to open a WebSocket connection...');
  websocket = new WebSocket(`ws://${hostname}/ws`);
  websocket.onopen    = onOpen;
  websocket.onclose   = onClose;
  websocket.onmessage = onMessage;
}
function onLoad(event) { initWebSocket(); }
function startWsHeartbeat(){
  // [FIX][SD+WEB] На случай повторного вызова — сначала чистим предыдущий интервал.
  stopWsHeartbeat();
  // [FIX][SD+WEB] Каждые 4 секунды отправляем лёгкий ping-команду на сервер.
  // Это создаёт входящий трафик для AsyncTCP и уменьшает риск _poll rx timeout.
  wsPingTimer = setInterval(function(){
    if(websocket && websocket.readyState===WebSocket.OPEN){
      sendWS('ping=1', true);
    }
  }, 4000);
}
function stopWsHeartbeat(){
  // [FIX][SD+WEB] Корректно снимаем интервал heartbeat при закрытии/пересоздании сокета.
  if(wsPingTimer){
    clearInterval(wsPingTimer);
    wsPingTimer = null;
  }
}
function pingUp(){
  if(!['/','/index.html'].includes(window.location.pathname)) return;
  clearTimeout(pongtimeout);
  // [FIX] Увеличен таймаут с 15 до 25 сек — при нагрузке на ESP32 (аудио-реконнект) сервер
  // может не успевать слать rssi; периодический keepalive на сервере (8 сек) + запас по времени.
  pongtimeout = setTimeout(() => {
    if(!bigplaylist){
      console.log('Connection closed');
      websocket.close();
    }
  }, 25000);
}
function onOpen(event) {
  console.log('Connection opened');
  pingUp();
  // [FIX][SD+WEB] Запускаем клиентский heartbeat только после успешного открытия сокета.
  startWsHeartbeat();
  // Полная подгрузка фрагмента — только если ещё не вставили разметку для этого URL
  // или первая попытка не успела (shellReady=false). Иначе — только досинхронизация.
  const pathname = window.location.pathname;
  if (!shellReady || pathname !== shellPathname) {
    continueLoading(playMode); // playMode in variables.js
  } else {
    syncCurrentViewAfterReconnect();
  }
  wserrcnt=0;
  // [FIX] Проверка загрузки theme.css: если CSS-переменные отсутствуют (ч/б UI),
  // перезагружаем theme.css динамически.
  setTimeout(function(){
    var ac = getComputedStyle(document.documentElement).getPropertyValue('--accent-color').trim();
    if(!ac || ac.length < 3) {
      console.log('theme.css missing, reloading...');
      var link = document.createElement('link');
      link.rel = 'stylesheet'; link.type = 'text/css';
      link.href = 'theme.css?_=' + Date.now();
      document.head.appendChild(link);
    }
  }, 3000);
}

function syncCurrentViewAfterReconnect(){
  const pathname = window.location.pathname;
  if(['/','/index.html'].includes(pathname)){
    sendWS('getindex=1');
    sendWS('gettrackfacts=1');
    return;
  }
  if(pathname=='/settings.html'){
    sendWS('getsystem=1');
    sendWS('getscreen=1');
    sendWS('gettimezone=1');
    sendWS('getweather=1');
    sendWS('getcontrols=1');
    sendWS('gettrackfacts=1');
    sendWS('getactive=1');
    return;
  }
  if(playMode !== 'player'){
    sendWS('getactive=1');
  }
}

function onClose(event) {
  wserrcnt++;
  clearTimeout(pongtimeout);
  // [FIX][SD+WEB] При закрытии сокета останавливаем heartbeat до следующего onOpen.
  stopWsHeartbeat();
  // [FIX] Показываем статус пользователю при потере соединения
  var meta = getId('meta');
  if(meta && wserrcnt > 1) meta.textContent = 'Переподключение... (' + wserrcnt + ')';
  // [FIX v2] Прогрессивный backoff таймаутов переподключения
  var delay = wserrcnt<=3 ? 3000 : wserrcnt<=7 ? 5000 : wserrcnt<=10 ? 10000 : 60000;
  wstimeout=setTimeout(initWebSocket, delay);
}
function secondToTime(seconds){
  if(seconds>=3600){
    return new Date(seconds * 1000).toISOString().substring(11, 19);
  }else{
    return new Date(seconds * 1000).toISOString().substring(14, 19);
  }
}
function showById(show,hide){
  show.forEach(item=>{ getId(item).classList.remove('hidden'); });
  hide.forEach(item=>{ getId(item).classList.add('hidden'); });
}
function onMessage(event) {
  // [FIX v2] Сбрасываем таймер «смерти» при ЛЮБОМ сообщении от сервера.
  // Раньше он сбрасывался только при rssi (раз в 2 сек), и на мобильных
  // браузерах таймер часто срабатывал раньше — WebSocket закрывался,
  // телефон пытался переподключиться, исчерпывал сокеты ESP32 → «Web умер».
  clearTimeout(pongtimeout);
  pingUp();
  try{
    const data = JSON.parse(escapeData(event.data));
    /*ir*/
    if(typeof data.ircode !== 'undefined'){
      getId('protocol').innerText=data.protocol;
      classEach('irrecordvalue', function(el){ if(el.hasClass("active")) el.innerText='0x'+data.ircode.toString(16).toUpperCase(); });
      return;
    }
    if(typeof data.irvals !== 'undefined'){
      classEach('irrecordvalue', function(el,i){ var val = data.irvals[i]; if(val>0) el.innerText='0x'+val.toString(16).toUpperCase(); else el.innerText=""; });
      return;
    }
    /*end ir*/
    if(typeof data.redirect !== 'undefined'){
      getId("mdnsnamerow").innerHTML=`<h3 style="line-height: 37px;color: #aaa; margin: 0 auto;">redirecting to ${data.redirect}</h3>`;
      setTimeout(function(){ window.location.href=data.redirect; }, 4000);
      return;
    }
    if(typeof data.playermode !== 'undefined') { //Web, SD
      // [FIX] Сбрасываем sdIndexingActive только при переходе в WEB-режим.
      // В SD-режиме НЕ сбрасываем — playermode может прийти раньше sdindexing=0.
      if(data.playermode != 'modesd') sdIndexingActive = false;
      // Оповещаем другие скрипты о начале/завершении потенциально тяжелой смены режима
      document.dispatchEvent(new CustomEvent('modeSwitching', { detail: true }));
      setTimeout(() => { document.dispatchEvent(new CustomEvent('modeSwitching', { detail: false })); }, 5000);

      modesd = data.playermode=='modesd';
      classEach('modeitem', function(el){ el.classList.add('hidden') });
      if(modesd) showById(['modesd', 'sdsvg'],['plsvg']); else showById(['modeweb','plsvg','bitinfo'],['sdsvg','snuffle']);
      showById(['volslider'],['sdslider']);
      // В SD оставляем избранное и жанры видимыми, если SD аппаратно активен (пины назначены и sdinit=1).
      const favBtn = getId('favoritesbutton');
      const genreBtn = getId('genrebutton');
      const sdActive = !!getId('playernav')?.classList.contains('sd'); // sdinit=1 ставит класс sd
      if(favBtn){
        if(sdActive || !modesd) favBtn.classList.remove('hidden'); else favBtn.classList.add('hidden');
      }
      if(genreBtn){
        if(sdActive || !modesd) genreBtn.classList.remove('hidden'); else genreBtn.classList.add('hidden');
      }
      getId('toggleplaylist').classList.remove('active');
      setPlaylistMod();
      // [FIX] Не загружаем плейлист, если SD-индексация ещё идёт —
      // файл может быть не готов (404). sdindexing=0 перезагрузит позже.
      if(!sdIndexingActive) {
        generatePlaylist(`http://${hostname}/api/playlist`+"?"+playlistmod);
      }
      // При смене режима принудительно обновляем обложку (сбрасываем в логотип).
      const logo = getId('logo');
      const cover = getId('cover-art-display');
      if(logo) logo.style.display = 'flex';
      if(cover){ cover.style.display = 'none'; cover.innerHTML = ''; }
      // Сбросить глобальное состояние TrackFacts (если доступно)
      if(typeof window.resetTrackFacts === 'function') window.resetTrackFacts();
      return;
    }
    if(typeof data.sdinit !== 'undefined') {
      if(data.sdinit==1) {
        getId('playernav').classList.add("sd");
        getId('volmbutton').classList.add("hidden");
      }else{
        getId('playernav').classList.remove("sd");
        getId('volmbutton').classList.remove("hidden");
      }
    }
    if(typeof data.sdindexing !== 'undefined') {
      const meta = getId('meta');
      if(data.sdindexing == 1) {
        sdIndexingActive = true;
        if(meta) meta.textContent = 'Индексация SD карты...';
      } else {
        // [FIX] Индексация завершена — сбрасываем флаг и перезагружаем плейлист.
        sdIndexingActive = false;
        if(meta) meta.textContent = 'Загрузка плейлиста...';
        // Перезагрузить плейлист, т.к. при первом playermode индексация ещё могла идти.
        setPlaylistMod();
        generatePlaylist('http://'+hostname+'/api/playlist?'+playlistmod);
      }
    }
    if(typeof data.sdpos !== 'undefined' && getId("sdpos")){
      if(data.sdtpos==0 && data.sdtend==0){
        getId("sdposvalscurrent").innerHTML="00:00";
        getId("sdposvalsend").innerHTML="00:00";
        getId("sdpos").value = data.sdpos;
        fillSlider(getId("sdpos"));
      }else{
        getId("sdposvalscurrent").innerHTML=secondToTime(data.sdtpos);
        getId("sdposvalsend").innerHTML=secondToTime(data.sdtend);
        getId("sdpos").value = data.sdpos;
        fillSlider(getId("sdpos"));
      }
      return;
    }
    if(typeof data.sdmin !== 'undefined' && getId("sdpos")){
      getId("sdpos").attr('min',data.sdmin); 
      getId("sdpos").attr('max',data.sdmax); 
      return;
    }
    if(typeof data.snuffle!== 'undefined'){
      if(data.snuffle==1){
        getId("snuffle").classList.add("active");
      }else{
        getId("snuffle").classList.remove("active");
      }
      return;
    }
    /* [v0.4.2] Обработка Toast-уведомлений от TrackFacts и системы */
    if(typeof data.toast !== 'undefined'){
      showToast(data.toast, data.isErr === 1);
      
      if (data.isErr === 1 && typeof data.toast === 'string' &&
          data.toast.indexOf('Запрос факта отклонён') !== -1) {
        closeFactsDialog();
      }
      return;
    }
    if(typeof data.sleep !== 'undefined'){
      updateSleepTimerState(data.sleep);
      return;
    }
    if(typeof data.payload !== 'undefined'){
      data.payload.forEach(item=> {
        setupElement(item.id, item.value);
      });
    }else{
      if(typeof data.current !== 'undefined') { setCurrentItem(data.current); return; }
      if(typeof data.file !== 'undefined') { setPlaylistMod(); generatePlaylist(data.file+"?"+playlistmod); sendWS('submitplaylistdone=1', true); return; }
        if(typeof data.act !== 'undefined'){
          // Если мы на settings — act подтверждает, что меню реально разморожено.
          if(window.location.pathname=='/settings.html'){
            settingsActReceived = true;
            clearTimeout(settingsActRetryTimer);
          }
          data.act.forEach(showclass=> { classEach(showclass, function(el) { el.classList.remove("hidden"); }); });
          return;
        }
      if(typeof data.mdns !== 'undefined'){
        const rhost = (hostname==`${data.mdns}.local`)?data.ipaddr:`${data.mdns}.local`;
        getId("radiolink").innerHTML=`<a href="http://${rhost}/settings.html">http://${rhost}/</a>`;
      }
      Object.keys(data).forEach(key=>{
        setupElement(key, data[key]);
      });
    }
  }catch(e){
    console.log("ws.onMessage error:", event.data);
  }
}
function escapeData(data){
  try {
    // Try to parse the JSON to see if it's valid
    const parsed = JSON.parse(data);
    // If parsing succeeds, return the original data
    return data;
  } catch(e) {
    // If parsing fails, try to escape unescaped quotes in string values
    // This regex handles the case where there are unescaped quotes inside JSON string values
    let fixed = data.replace(/"([^"]*)":\s*"([^"]*)"/g, (match, key, value) => {
      // Escape any unescaped quotes in the value
      const escapedValue = value.replace(/"/g, '\\"');
      return `"${key}": "${escapedValue}"`;
    });
    return fixed;
  }
}
function getId(id,patent=document){
  return patent.getElementById(id);
}
function classEach(classname, callback) {
  document.querySelectorAll(`.${classname}`).forEach((item, index) => callback(item, index));
}
function quoteattr(s) {
  return ('' + s)
    .replace(/&/g, '&amp;')
    .replace(/'/g, '&apos;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;');
}
HTMLElement.prototype.attr = function(name, value=null){
  if(value!==null){
    return this.setAttribute(name, value);
  }else{
    return this.getAttribute(name);
  }
}
HTMLElement.prototype.hasClass = function(className){
  return this.classList.contains(className);
}
function fillSlider(sl){
  const slaveid = sl.dataset.slaveid;
  const value = (sl.value-sl.min)/(sl.max-sl.min)*100;
  if(slaveid) getId(slaveid).innerText=sl.value;
  sl.style.background = 'linear-gradient(to right, var(--accent-dark) 0%,  var(--accent-dark) ' + value + '%, var(--odd-bg-color) ' + value + '%, var(--odd-bg-color) 100%)';
}
function isSystemMetaValue(value){
  if(!value) return false;
  if(value.startsWith('[')) return true;
  if(value.indexOf('Индексация SD') !== -1) return true;
  if(value.indexOf('Загрузка плейлиста') !== -1) return true;
  return false;
}
function setupElement(id, value){
  //console.log(`Updating element: id=${id}, value=${value}`); // Debug log
  const element = getId(id);
  if(id=="rssi"){ /* pongtimeout теперь сбрасывается в onMessage() для всех сообщений */ }
  
  /* [v0.4.2] Toast notification logic */
  window.showToast = function(msg, isError = false) {
    let container = getId('toast-container');
    if (!container) {
      container = document.createElement('div');
      container.id = 'toast-container';
      container.style.cssText = 'position:fixed;bottom:20px;left:50%;transform:translateX(-50%);z-index:9999;display:flex;flex-direction:column;align-items:center;pointer-events:none;';
      document.body.appendChild(container);
    }
    
    const toast = document.createElement('div');
    toast.style.cssText = `
      background:${isError ? '#d32f2f' : '#333'};
      color:#fff;
      padding:10px 20px;
      margin-top:10px;
      border-radius:25px;
      font-size:14px;
      box-shadow:0 4px 12px rgba(0,0,0,0.3);
      opacity:0;
      transition:opacity 0.5s, transform 0.5s;
      transform:translateY(20px);
      white-space:nowrap;
    `;
    toast.innerText = msg;
    container.appendChild(toast);
    
    // Animate in
    setTimeout(() => { toast.style.opacity = '1'; toast.style.transform = 'translateY(0)'; }, 10);
    
    // Remove after 4 seconds
    setTimeout(() => {
      toast.style.opacity = '0';
      toast.style.transform = 'translateY(-20px)';
      setTimeout(() => container.removeChild(toast), 500);
    }, 4000);
  };

  // [FIX] Обновляем глобальный флаг trackFactsEnabled при получении tfen (до проверки element, т.к. на player page элемента #tfen нет)
  if(id === 'tfen') trackFactsEnabled = !!value;
  // Runtime-переключатель обложек на TFT показываем только когда фича разрешена в сборке (DISPLAY_COVERS_ENABLE=true).
  if (id === 'covopt') {
    const covCb = getId('coven');
    if (covCb) {
      if (value) covCb.classList.remove('hidden');
      else covCb.classList.add('hidden');
    }
    return;
  }

  if(element){
    if(id=="heap"){
      // Ширина — фактический процент буфера.
      element.style.width=`${value}%`;
      // Цвет — по заданным порогам; сама плавность перехода обеспечивается CSS transition у #heap.
      element.style.backgroundColor = getHeapColorByPercent(value);
      return;
    }
    if(element.classList.contains("checkbox")){
      element.classList.remove("checked");
      if(value) element.classList.add("checked");
    }
    if(element.classList.contains("classchange")){
      element.attr("class", "classchange");
      element.classList.add(value);

      if(id === 'playerwrap' && value === 'stopped') {
        // Сброс обложки на логотип
        var logoEl = getId('logo');
        var coverEl = getId('cover-art-display');
        if(logoEl) logoEl.style.display = 'flex';
        if(coverEl){ coverEl.style.display = 'none'; coverEl.innerHTML = ''; }
        if(typeof window.resetTrackFacts === 'function') window.resetTrackFacts();
      }
      // [FIX Задача 2] При начале воспроизведения — если плейлист пуст, перезагружаем его.
      // Покрывает случай: страница загрузилась во время индексации SD, плейлист был пустой,
      // событие sdindexing=0 пропущено или пришло раньше готовности данных.
      if(id === 'playerwrap' && value === 'playing') {
        setTimeout(function() {
          var pl = getId('playlist');
          if(pl && pl.querySelectorAll('li').length === 0) {
            setPlaylistMod();
            generatePlaylist('http://'+hostname+'/api/playlist?'+playlistmod);
          }
        }, 2000); // 2сек задержка — даём серверу время подготовить данные
      }
    }
    if(element.classList.contains("text")){
      if(id=='meta'){
        var now = Date.now();
        if(now - lastMetaUpdate < META_NAMESET_THROTTLE_MS) return;
        lastMetaUpdate = now;
      }
      if(id=='nameset'){
        var now = Date.now();
        if(now - lastNamesetUpdate < META_NAMESET_THROTTLE_MS) return;
        lastNamesetUpdate = now;
      }
      if(id=='meta'){
        if(sdIndexingActive && !isSystemMetaValue(value)) return;
        if(getId('playerwrap') && getId('playerwrap').classList.contains('stopped')) {
          if(value && value.length > 0 && !value.startsWith('[')) return;
        }
      }
      let finalValue = value;
      if(id=='nameset'){
        if(value && value.trim().length > 0) {
          lastNamesetText = value;
        } else if(lastNamesetText) {
          finalValue = lastNamesetText;
        }
      }
      element.innerText=finalValue;
      // setCurrentItem при meta/nameset не вызываем — подсветка/скролл только по data.current и при загрузке плейлиста
    }
    if(element.type==='text' || element.type==='number' || element.type==='password'){
      element.value=value;
    }
    if(element.tagName === 'SELECT'){
      element.value=value;
      if(id=='tfprovider') updateKeyDesc(false);
    }
    if(element.type==='range'){
      if(id=='volume') element.max = 100;
      element.value=value;
      fillSlider(element);
    }
  }
}

// ============================================================================
// Sleep Timer UI:
// - переключатель ALL OFF на странице SYSTEM,
// - кнопка-индикатор таймера на главной,
// - динамическая смена цвета/секторов и мигание в последнюю минуту.
// ============================================================================
function updateSleepTimerState(payload) {
  if (!payload) return;
  sleepTimerState.m = Number(payload.m || 0);
  sleepTimerState.left = Number(payload.left || 0);
  sleepTimerState.active = Number(payload.active || 0);
  sleepTimerState.alloff = Number(payload.alloff || 0);
  sleepTimerState.supported = Number(payload.supported || 0);

  const allOffCb = getId('stao');
  if (allOffCb) {
    if (sleepTimerState.supported) allOffCb.classList.remove('hidden');
    else allOffCb.classList.add('hidden');

    allOffCb.classList.remove('checked');
    if (sleepTimerState.alloff) allOffCb.classList.add('checked');
  }

  renderSleepTimerButton();
}

function renderSleepTimerButton() {
  var btn = getId('sleeptimerbtn');
  if (!btn) return;

  var sectors = [
    btn.querySelector('.sleep-sector.s1'),
    btn.querySelector('.sleep-sector.s2'),
    btn.querySelector('.sleep-sector.s3'),
    btn.querySelector('.sleep-sector.s4')
  ];
  var moon = btn.querySelector('.sleep-moon');
  var ring = btn.querySelector('.sleep-ring');

  // Оставшееся время в секундах (если активен — реальный остаток, иначе — выбранный пресет)
  var sec = sleepTimerState.active ? sleepTimerState.left : (sleepTimerState.m * 60);
  var min = Math.ceil(sec / 60);  // оставшиеся минуты, округлённые вверх
  var color = '#ff3f2f';  // красный по умолчанию
  var filled = 0;  // сколько секторов закрашено (0..4)

  // === Логика визуализации по спецификации promtT.md ===
  // Выключен: 0 секторов, просто луна.
  // <=15 мин: 1/4 красный, <=30 мин: 1/2 красный, <=45 мин: 3/4 красный,
  // <=60 мин: полный красный, <=90 мин: полный жёлтый, >90 мин: полный зелёный.
  if (sec <= 0) {
    filled = 0;
  } else if (min > 90) {
    // 120 мин (2 часа) — полностью зелёный
    color = '#39d964'; filled = 4;
  } else if (min > 60) {
    // 90 мин (1.5 часа) — полностью жёлтый
    color = '#f2cb2f'; filled = 4;
  } else if (min > 45) {
    // 60 мин — полностью красный
    color = '#ff3f2f'; filled = 4;
  } else if (min > 30) {
    // 45 мин — 3/4 красный
    color = '#ff3f2f'; filled = 3;
  } else if (min > 15) {
    // 30 мин — 1/2 красный
    color = '#ff3f2f'; filled = 2;
  } else {
    // 15 мин и меньше — 1/4 красный
    color = '#ff3f2f'; filled = 1;
  }

  // Закрашиваем секторы
  for (var i = 0; i < sectors.length; i++) {
    if (!sectors[i]) continue;
    if (i < filled) {
      sectors[i].style.opacity = '0.95';
      sectors[i].style.fill = color;
    } else {
      sectors[i].style.opacity = '0';
    }
  }

  // Кольцо: цвет текущего состояния или золотистый по умолчанию
  if (ring) ring.style.stroke = sec > 0 ? color : 'var(--accent-color)';
  // Луна: видна когда таймер выключен, скрыта когда активен (секторы вместо неё)
  if (moon) moon.style.opacity = sec > 0 ? '0' : '0.4';

  // Мигание на последней минуте (от 60 до 0 секунд) — каждые 2 секунды
  if (sleepTimerState.active && sec > 0 && sec <= 60) {
    var blinkOn = (Math.floor(sec / 2) % 2) === 0;
    btn.style.opacity = blinkOn ? '1' : '0.3';
  } else {
    btn.style.opacity = '1';
  }
}
/***--- playlist ---***/
function setCurrentItem(item){
  currentItem=item;
  const playlist = getId("playlist");
  let topPos = 0, lih = 0;
  playlist.querySelectorAll('li').forEach((item, index)=>{
      // Не перезаписываем class целиком: иначе теряются служебные классы фильтров
      // (например, .search-hidden), и поиск "ломается" после первого запуска/PLAY.
      item.classList.add("play");
      item.classList.remove("active");
      if(index+1==currentItem){
          item.classList.add("active");
          topPos = item.offsetTop;
          lih = item.offsetHeight;
      }
  });
  // Мгновенный скролл: 'smooth' при частых обновлениях давал дёргание при движении курсора
  playlist.scrollTo({ top: (topPos-playlist.offsetHeight/2+lih/2), left: 0, behavior: 'auto' });
}
function initPLEditor(){
  ple= getId('pleditorcontent');
  if(!ple) return;
  let html='';
  ple.innerHTML="";
  pllines = getId('playlist').querySelectorAll('li');
  pllines.forEach((item,index)=>{
    let pName = item.dataset.name;
    let pUrl = item.dataset.url;
    let pOvol = item.dataset.ovol;
    let pGenre = item.dataset.genre || '';
    let pFavorite = item.dataset.favorite || '0';
    
    html+=`<li class="pleitem" id="${'plitem'+index}"><span class="grabbable" draggable="true">${("00"+(index+1)).slice(-3)}</span>
      <span class="pleinput plecheck"><input type="checkbox" class="plcb" /></span>
      <input class="pleinput plename" type="text" value="${quoteattr(pName)}" maxlength="140" />
      <input class="pleinput pleurl" type="text" value="${pUrl}" maxlength="140" />
      <span class="pleinput pleplay" data-command="preview">&#9658;</span>
      <input class="pleinput pleovol" type="number" min="-64" max="64" step="1" value="${pOvol}" />
      <input class="pleinput plegenre" type="text" value="${pGenre}" maxlength="50" onchange="autoSaveGenre(this)" />
      <input class="pleinput plefavorite" type="number" min="0" max="1" step="1" value="${pFavorite}" />
      </li>`;
  });
  ple.innerHTML=html;
}

/* AUTO-SAVE FUNCTION for Genres */
function autoSaveGenre(input) {
  // Update the dataset of the corresponding playlist item
  let liIndex = input.closest('li').id.replace('plitem', '');
  let playlistItem = getId('playlist').children[liIndex];
  if (playlistItem) {
    playlistItem.dataset.genre = input.value;
    playlistItem.setAttribute('data-genre', input.value);
  }
  // Trigger silent save
  submitPlaylist(true); 
}

function handlePlaylistData(fileData) {
  const ul = getId('playlist');
  ul.innerHTML='';
  if (!fileData) return;
  const lines = fileData.split('\n');
  let li='', html='';
  for(var i = 0;i < lines.length;i++){
    let line = lines[i].split('\t');
    if(line.length>=3){
      // Один атрибут class — иначе два class="..." дают невалидный HTML: браузер оставляет
      // только первый, и строка «текущей» станции теряет .play → клик не попадает в playItem().
      const activeClass = (i+1==currentItem) ? ' active' : '';
      const genre = line[3] ? line[3].trim() : '';
      const favorite = line[4] ? line[4].trim() : '0';
      const favClass = favorite === '1' ? 'active' : '';
      
      li=`<li class="play${activeClass}" attr-id="${i+1}" data-name="${line[0].trim()}" data-url="${line[1].trim()}" data-ovol="${line[2].trim()}" data-genre="${genre}" data-favorite="${favorite}">
          <button class="favorite-btn ${favClass}" onclick="toggleFavorite(this, event)"></button>
          <span class="text">${line[0].trim()}</span>
          <span class="count">${i+1}</span>
          </li>`;
      html += li;
    }
  }
  ul.innerHTML=html;
  setCurrentItem(currentItem);
  // Если поиск уже открыт/введён, применяем фильтр повторно после перезагрузки плейлиста.
  // Это сохраняет результаты поиска и позволяет делать повторные поиски без F5.
  filterPlaylistBySearch();
  updateGenreList();
  if(!modesd) initPLEditor();
  bigplaylist = false;
}

function generatePlaylist(path){
  // [FIX v3] Защита от двойного вызова: если плейлист уже грузится — пропускаем.
  if(bigplaylist) return;
  path = path.replace(/:\/\/.+?\//, `://${hostname}/`);
  var plEl = getId('playlist');
  var savedHtml = plEl.innerHTML;
  plEl.innerHTML='<div id="progress"><span id="loader"></span></div>';
  bigplaylist = true;
  fetch(path).then(response => response.text()).then(plcontent => {
    handlePlaylistData(plcontent);
  }).catch(() => {
    // При ошибке (таймаут, занятый LittleFS при избранном и т.д.) не очищаем список — восстанавливаем.
    plEl.innerHTML = savedHtml;
    bigplaylist = false;
  });
}

/* GENRE & FAVORITES LOGIC */
function extractGenresFromPlaylist() {
  const items = getId('playlist').querySelectorAll('li');
  const genres = new Set();
  items.forEach(item => {
    const genre = item.dataset.genre;
    if (genre) genres.add(genre);
  });
  return Array.from(genres).sort();
}

function updateGenreList() {
  const genreList = getId('genrelist');
  if(!genreList) return;
  const genres = extractGenresFromPlaylist();
  genreList.innerHTML = '<li data-genre="all">All Genres</li>';
  genres.forEach(genre => {
    if (genre && genre.trim() !== '') {
      const li = document.createElement('li');
      li.textContent = genre;
      li.setAttribute('data-genre', genre);
      genreList.appendChild(li);
    }
  });
  
  genreList.querySelectorAll('li').forEach(li => {
    li.addEventListener('click', function(e) {
      filterByGenre(this.getAttribute('data-genre'));
      getId('genredialog').style.display = 'none';
      e.stopPropagation();
    });
  });
}

function filterByGenre(genre) {
  const items = getId('playlist').querySelectorAll('li');
  // Reset favorites filter when changing genre
  getId('favoritesbutton').classList.remove('active');
  
  items.forEach(item => {
    const itemGenre = item.dataset.genre;
    if (genre === 'all' || itemGenre === genre) {
      item.style.display = 'flex';
    } else {
      item.style.display = 'none';
    }
  });
}

function toggleFavorite(btn, e) {
  e.stopPropagation();
  btn.classList.toggle('active');
  const li = btn.closest('li');
  const newVal = btn.classList.contains('active') ? '1' : '0';
  
  // Update Dataset
  li.setAttribute('data-favorite', newVal);
  li.dataset.favorite = newVal;
  
  // SYNC WITH EDITOR
  // We need to update the corresponding input in the editor HTML even if it is hidden
  const index = Array.from(li.parentNode.children).indexOf(li);
  const editorLi = getId('pleditorcontent').children[index];
  if(editorLi) {
    const inputs = editorLi.querySelectorAll('input');
    // Assuming Favorite is the last input or specific class
    const favInput = editorLi.querySelector('.plefavorite');
    if(favInput) favInput.value = newVal;
  }

  const itemId = parseInt(li.getAttribute('attr-id'), 10);
  const sdActive = modesd || (getId('playernav') && getId('playernav').classList.contains('sd'));
  if(sdActive && itemId > 0) {
    sendWS(`favset=${itemId},${newVal}`, true);
  } else {
    submitPlaylist(true);
  }
}

function toggleFavoritesFilter() {
    const btn = getId('favoritesbutton');
    btn.classList.toggle('active');
    const showOnlyFavs = btn.classList.contains('active');
    
    const items = getId('playlist').querySelectorAll('li');
    items.forEach(item => {
        const isFav = item.dataset.favorite === '1';
        if (showOnlyFavs) {
            item.style.display = isFav ? 'flex' : 'none';
        } else {
            item.style.display = 'flex';
        }
    });
}

/* === ПОИСК ПО ПЛЕЙЛИСТУ ===
   Открывает поле ввода над плейлистом; при вводе текста скрывает неподходящие
   элементы <li> через класс .search-hidden. При закрытии — сбрасывает фильтр. */
function toggleSearchDialog() {
  var dlg = getId('searchdialog');
  var inp = getId('searchinput');
  if(!dlg) return;
  if(dlg.classList.contains('hidden')) {
    // Открытие диалога поиска
    dlg.classList.remove('hidden');
    getId('searchbtn').classList.add('active');
    inp.value = '';
    inp.focus();
  } else {
    closeSearchDialog();
  }
}

function closeSearchDialog() {
  var dlg = getId('searchdialog');
  var inp = getId('searchinput');
  if(!dlg) return;
  dlg.classList.add('hidden');
  getId('searchbtn').classList.remove('active');
  inp.value = '';
  // Сбрасываем фильтр поиска — показываем все элементы
  var items = getId('playlist').querySelectorAll('li');
  items.forEach(function(item) { item.classList.remove('search-hidden'); });
}

function filterPlaylistBySearch() {
  var inp = getId('searchinput');
  if(!inp) return;
  var query = inp.value.toLowerCase().trim();
  var items = getId('playlist').querySelectorAll('li');
  items.forEach(function(item) {
    if(!query) {
      // Пустой запрос — показываем все
      item.classList.remove('search-hidden');
    } else {
      // Ищем совпадение в названии трека (data-name) или тексте элемента
      var name = (item.dataset.name || item.textContent || '').toLowerCase();
      if(name.indexOf(query) !== -1) {
        item.classList.remove('search-hidden');
      } else {
        item.classList.add('search-hidden');
      }
    }
  });
}

// Подключаем обработчик ввода при загрузке страницы
(function() {
  // Ждём DOM-готовности (#searchinput может добавляться через innerHTML)
  var _searchInitInterval = setInterval(function() {
    var inp = getId('searchinput');
    if(inp) {
      clearInterval(_searchInitInterval);
      // Фильтрация при каждом нажатии клавиши
      inp.addEventListener('input', filterPlaylistBySearch);
      // Escape закрывает поиск
      inp.addEventListener('keydown', function(e) {
        if(e.key === 'Escape') closeSearchDialog();
      });
    }
  }, 500);
})();

function plAdd(){
  let ple=getId('pleditorcontent');
  let plitem = document.createElement('li');
  let cnt=ple.getElementsByTagName('li');
  plitem.attr('class', 'pleitem');
  plitem.attr('id', 'plitem'+(cnt.length));
  plitem.innerHTML = '<span class="grabbable" draggable="true">'+("00"+(cnt.length+1)).slice(-3)+'</span>\
      <span class="pleinput plecheck"><input type="checkbox" /></span>\
      <input class="pleinput plename" type="text" value="" maxlength="140" />\
      <input class="pleinput pleurl" type="text" value="" maxlength="140" />\
      <span class="pleinput pleplay" data-command="preview">&#9658;</span>\
      <input class="pleinput pleovol" type="number" min="-30" max="30" step="1" value="0" />\
      <input class="pleinput plegenre" type="text" value="" maxlength="50" onchange="autoSaveGenre(this)" />\
      <input class="pleinput plefavorite" type="number" min="0" max="1" step="1" value="0" />';
  ple.appendChild(plitem);
  ple.scrollTo({
    top: ple.scrollHeight,
    left: 0,
    behavior: 'smooth'
  });
}
function plRemove(){
  let items=getId('pleditorcontent').getElementsByTagName('li');
  let pass=[];
  for (let i = 0; i <= items.length - 1; i++) {
    if(items[i].getElementsByTagName('span')[1].getElementsByTagName('input')[0].checked) {
      pass.push(items[i]);
    }
  }
  if(pass.length==0) {
    alert('Choose something first');
    return;
  }
  for (var i = 0; i < pass.length; i++)
  {
    pass[i].remove();
  }
  items=getId('pleditorcontent').getElementsByTagName('li');
  for (let i = 0; i <= items.length-1; i++) {
    items[i].getElementsByTagName('span')[0].innerText=("00"+(i+1)).slice(-3);
  }
}
function submitPlaylist(silent = false){
  var items=getId("pleditorcontent").getElementsByTagName("li");
  // [FIX] В SD-режиме редактор может быть пустой — инициализируем из плейлиста
  if(items.length === 0) {
    initPLEditor();
    items = getId("pleditorcontent").getElementsByTagName("li");
    if(items.length === 0) return; // Плейлист пуст, нечего сохранять
  }
  var output="";
  for (var i = 0; i <= items.length - 1; i++) {
    inputs=items[i].getElementsByTagName("input");
    if(inputs[1].value == "" || inputs[2].value == "") continue;
    let ovol = inputs[3].value;
    if(ovol < -30) ovol = -30;
    if(ovol > 30) ovol = 30;
    let genre = inputs[4] ? inputs[4].value : '';
    let favorite = inputs[5] ? inputs[5].value : '0';
    output+=inputs[1].value+"\t"+inputs[2].value+"\t"+inputs[3].value+"\t"+genre+"\t"+favorite+"\n";
  }
  let file = new File([output], "tempplaylist.csv",{type:"text/plain;charset=utf-8", lastModified:new Date().getTime()});
  let container = new DataTransfer();
  container.items.add(file);
  let fileuploadinput=getId("file-upload");
  fileuploadinput.files = container.files;
  doPlUpload(fileuploadinput, silent);
  
  if(!silent) {
    toggleTarget(0, 'pleditorwrap');
  }
}
function setPlaylistMod(){
  playlistmod = new Date().getTime();
  localStorage.setItem("playlistmod", playlistmod);
}
function doPlUpload(finput, silent) {
  // [FIX] silent=true (из toggleFavorite) — не останавливаем плеер.
  // silent=false (Save из редактора) — останавливаем, т.к. порядок станций мог измениться.
  sendWS(silent ? "submitplaylistsilent=1" : "submitplaylist=1", true);
  var formData = new FormData();
  formData.append("plfile", finput.files[0]);
  var xhr = new XMLHttpRequest();
  xhr.open("POST",`http://${hostname}/upload`,true);
  xhr.send(formData);
  finput.value = '';
}
/***--- eof playlist ---***/
function toggleTarget(el, id){
  const target = getId(id);
  if(id=='pleditorwrap'){
    audiopreview.pause();
    audiopreview.src='';
    getId('previewinfo').innerHTML='';
    // Re-init editor to capture current favorite states if opening
    if(target.classList.contains("hidden")) initPLEditor();
  }
  if(target){
    if(id=='pleditorwrap' && modesd) {
      getId('sdslider').classList.toggle('hidden');
      getId('volslider').classList.toggle('hidden');
      getId('bitinfo').classList.toggle('hidden');
      getId('snuffle').classList.toggle('hidden');
    }else target.classList.toggle("hidden");
    getId(target.dataset.target).classList.toggle("active");
  }
}
function checkboxClick(cb, command){
  cb.classList.toggle("checked");
  sendWS(`${command}=${cb.classList.contains("checked")?1:0}`);
}
function sliderInput(sl, command){
  sendWS(`${command}=${sl.value}`);
  fillSlider(sl);
}
function handleWiFiData(fileData) {
  if (!fileData) return;
  var lines = fileData.split('\n');
  for(var i = 0;i < lines.length;i++){
    let line = lines[i].split('\t');
    if(line.length==2){
      getId("ssid"+i).value=line[0].trim();
      getId("pass"+i).attr('data-pass', line[1].trim());
    }
  }
}
function getWiFi(path){
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (xhr.readyState == 4) {
      if (xhr.status == 200) {
        handleWiFiData(xhr.responseText);
      } else {
        handleWiFiData(null);
      }
    }
  };
  xhr.open("GET", path);
  xhr.send(null);
}
function applyTZ(){
  sendWS("tzh="+getId("tzh").value);
  sendWS("tzm="+getId("tzm").value);
  sendWS("sntp2="+getId("sntp2").value);
  sendWS("sntp1="+getId("sntp1").value);
}
function rebootSystem(info){
  getId("settingscontent").innerHTML=`<h2>${info}</h2>`;
  getId("settingsdone").classList.add("hidden");
  getId("navigation").classList.add("hidden");
  setTimeout(function(){ window.location.href=`http://${hostname}/`; }, 5000);
}
function submitWiFi(){
  var output="";
  var items=document.getElementsByClassName("credential");
  for (var i = 0; i <= items.length - 1; i++) {
    inputs=items[i].getElementsByTagName("input");
    if(inputs[0].value == "") continue;
    let ps=inputs[1].value==""?inputs[1].dataset.pass:inputs[1].value;
    output+=inputs[0].value+"\t"+ps+"\n";
  }
  if(output!=""){ // Well, let's say, quack.
    let file = new File([output], "tempwifi.csv",{type:"text/plain;charset=utf-8", lastModified:new Date().getTime()});
    let container = new DataTransfer();
    container.items.add(file);
    let fileuploadinput=getId("file-upload");
    fileuploadinput.files = container.files;
    var formData = new FormData();
    formData.append("wifile", fileuploadinput.files[0]);
    var xhr = new XMLHttpRequest();
    xhr.open("POST",`http://${hostname}/upload`,true);
    xhr.send(formData);
    fileuploadinput.value = '';
    getId("settingscontent").innerHTML="<h2>Settings saved. Rebooting...</h2>";
    getId("settingsdone").classList.add("hidden");
    getId("navigation").classList.add("hidden");
    setTimeout(function(){ window.location.href=`http://${hostname}/`; }, 10000);
  }
}
function playItem(target){
  const item = target.attr('attr-id');
  setCurrentItem(item)
  sendWS(`play=${item}`, true);
}
function hideSpinner(){
  getId("progress").classList.add("hidden");
  getId("content").classList.remove("hidden");
}
function changeMode(el){
  const cmd = el.dataset.command;
  //setPlaylistMod();
  el.classList.add('hidden');
  if(cmd=='web') getId('modesd').classList.remove('hidden');
  else getId('modeweb').classList.remove('hidden');
  sendWS("newmode="+(cmd=="web"?0:1), true);
}
function toggleSnuffle(){
  let el = getId('snuffle');
  el.classList.toggle('active');
  sendWS("snuffle="+el.classList.contains('active'));
}
function previewInfo(text, url='', error=false){
  const previewinfo=getId('previewinfo');
  previewinfo.classList.remove('error');
  if(url!='') previewinfo.innerHTML=`${text} <a href="${url}" target="_blank">${url}</a>`;
  else previewinfo.innerHTML=`${text}`;
  if(error) previewinfo.classList.add('error');
}
const PREVIEW_TIMEOUT = 4000;
function playPreview(root) {
  const streamUrl=root.getElementsByClassName('pleurl')[0].value;
  if(root.hasClass('active')){ root.classList.remove('active'); audiopreview.pause(); previewInfo('Stop playback:', streamUrl); return; }
  classEach('pleitem', function(el){ el.classList.remove('active') });
  if(streamUrl=='' || !audiopreview) { previewInfo("No streams available.", '', true); return; }
  previewInfo('Attempting to play:', streamUrl);
  audiopreview.src = streamUrl;
  audiopreview.load();
  let isTimeout = false;
  const timeout = setTimeout(() => { isTimeout = true; previewInfo("Connection timeout", streamUrl, true); root.classList.remove('active'); audiopreview.pause(); audiopreview.src = ''; return; }, PREVIEW_TIMEOUT);
  const onCanPlay = () => { if (!isTimeout) { clearTimeout(timeout); previewInfo('Playback', streamUrl); root.classList.add('active'); audiopreview.play().catch(err => { previewInfo("Playback error:", streamUrl, true); root.classList.remove('active'); return; }); }  };
  const onError = () => { if (!isTimeout) { clearTimeout(timeout); root.classList.remove('active'); previewInfo("Error loading stream:", streamUrl, true); audiopreview.src = ''; return; } };
  audiopreview.addEventListener("canplay", onCanPlay, { once: true });
  audiopreview.addEventListener("error", onError, { once: true });
}

function toggleGenreDialog() {
  const dialog = getId('genredialog');
  const genreBtn = getId('genrebutton');
  const isVisible = dialog.style.display === 'block';
  
  genreBtn.classList.remove('active');
  
  if (isVisible) {
    dialog.style.display = 'none';
  } else {
        const equalizer = getId('equalizerbg');
    if (equalizer && !equalizer.classList.contains('hidden')) {
      equalizer.classList.add('hidden');
      getId('eqalbutton').classList.remove('active');
    }
    
        dialog.style.display = 'block';
     genreBtn.classList.add('active');
  }
}

// Tabs Dialog - показ фактов о треке по клику на обложке
let factsDialogTimer = null;
let factsDialogPollTimer = null;
window.factsDialogOpen = false; // [FIX] Флаг — когда диалог открыт, факт не показывается в области трека
// [FIX] Глобальный кеш фактов для диалога:
// хранит последний валидный набор фактов по текущему треку, чтобы при открытии диалога
// сначала показывать уже известные данные, а не сразу инициировать новый сетевой запрос.
window.trackFactsCache = window.trackFactsCache || { title: '', facts: [], updatedAt: 0 };

function setTrackFactsCache(title, facts) {
  // Нормализуем входной массив: только непустые строки.
  const safeFacts = (facts || []).filter(f => f && f.trim().length > 0);
  // Обновляем единый кеш для всех частей WebUI (строка тега + диалог).
  window.trackFactsCache = {
    title: title || '',
    facts: safeFacts,
    updatedAt: Date.now()
  };
}

function closeFactsDialog() {
  // Централизованно закрываем диалог фактов и очищаем все его таймеры.
  // Это исключает дублирование кода и ситуации, когда один из таймеров остаётся активным.
  const dialog = getId('factsdialog');
  if (dialog) {
    dialog.style.display = 'none';
  }
  window.factsDialogOpen = false;
  if (factsDialogTimer) {
    clearTimeout(factsDialogTimer);
    factsDialogTimer = null;
  }
  if (factsDialogPollTimer) {
    clearTimeout(factsDialogPollTimer);
    factsDialogPollTimer = null;
  }
}

function toggleFactsDialog() {
  const dialog = getId('factsdialog');
  if (!dialog) return;
  
  // [FIX] Если факты выключены в настройках — не открываем диалог
  if (!trackFactsEnabled) return;
  
  const isVisible = dialog.style.display === 'block';
  
  // Очистить таймер автозакрытия
  if (factsDialogTimer) {
    clearTimeout(factsDialogTimer);
    factsDialogTimer = null;
  }
  // Очистить таймер фонового опроса фактов в диалоге.
  if (factsDialogPollTimer) {
    clearTimeout(factsDialogPollTimer);
    factsDialogPollTimer = null;
  }
  
  if (isVisible) {
    closeFactsDialog();
  } else {
    // Закрываем другие диалоги
    window.factsDialogOpen = true;
    const genreDialog = getId('genredialog');
    if (genreDialog) genreDialog.style.display = 'none';
    
    const equalizer = getId('equalizerbg');
    if (equalizer && !equalizer.classList.contains('hidden')) {
      equalizer.classList.add('hidden');
    }
    
    // Показать диалог
    dialog.style.display = 'block';
    
    // Загрузить факты
    loadFactsForDialog();
    
    // Автозакрытие через 30 секунд
    factsDialogTimer = setTimeout(() => {
      closeFactsDialog();
    }, 30000);
  }
}

async function pollFactsDialogUntilReady(maxMs) {
  // Держим диалог "живым" после ручного запроса:
  // регулярно перепрашиваем API, чтобы не оставлять вечное "⏳ Запрос факта...".
  const startedAt = Date.now();
  const content = getId('factsdialog-content');
  const header = getId('factsdialog-header');
  if (!content) return;

  // Локальная функция одного шага опроса.
  const tick = async () => {
    // Если диалог уже закрыт — прекращаем опрос.
    if (!window.factsDialogOpen) {
      factsDialogPollTimer = null;
      return;
    }

    try {
      const response = await fetch('/api/current-fact?t=' + Date.now());
      if (!response.ok) {
        content.innerHTML = '<p class="no-facts">Факты недоступны</p>';
        factsDialogPollTimer = null;
        return;
      }
      const data = await response.json();
      const facts = (data.facts || []).filter(f => f && f.trim().length > 0);
      if (header && data.title) {
        header.innerHTML = '💡 ' + escapeHtmlFacts(data.title);
      }

      // Если факт уже получен — показываем и завершаем опрос.
      if (facts.length > 0) {
        setTrackFactsCache(data.title || '', facts);
        content.innerHTML = facts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('');
        factsDialogPollTimer = null;
        return;
      }

      // Если запрос ещё в процессе и мы не вышли по таймауту — продолжаем polling.
      const elapsed = Date.now() - startedAt;
      if (data.pending && elapsed < maxMs) {
        const leftSec = Math.max(1, Math.ceil((maxMs - elapsed) / 1000));
        content.innerHTML = '<p class="no-facts">⏳ Запрос факта... (' + leftSec + 'с)</p>';
        factsDialogPollTimer = setTimeout(tick, 1000);
        return;
      }

      // Если pending снят или вышли по времени — показываем причину/финальный статус.
      if (data.status && data.status.length > 0) {
        content.innerHTML = '<p class="no-facts">' + escapeHtmlFacts(data.status) + '</p>';
      } else {
        content.innerHTML = '<p class="no-facts">Запрос факта не выполнен. Попробуйте позже.</p>';
      }
      factsDialogPollTimer = null;
    } catch (e) {
      content.innerHTML = '<p class="no-facts">Ошибка загрузки фактов</p>';
      factsDialogPollTimer = null;
    }
  };

  await tick();
}

async function loadFactsForDialog() {
  const content = getId('factsdialog-content');
  const header = getId('factsdialog-header');
  if (!content) return;
  
  content.innerHTML = '<p class="loading">Загрузка...</p>';
  
  try {
    const response = await fetch('/api/current-fact?t=' + Date.now());
    if (!response.ok) {
      content.innerHTML = '<p class="no-facts">Факты недоступны</p>';
      return;
    }
    
    const data = await response.json();
    const facts = (data.facts || []).filter(f => f && f.trim().length > 0);
    // Читаем локальный кеш, накопленный в цикле TrackFacts.
    const cached = window.trackFactsCache || { title: '', facts: [] };
    const cachedFacts = (cached.facts || []).filter(f => f && f.trim().length > 0);
    
    // Обновляем заголовок с названием трека
    if (header && data.title) {
      header.innerHTML = '💡 ' + escapeHtmlFacts(data.title);
    }
    
    if (facts.length === 0) {
      // [FIX] Cache-first: если API временно отдал пусто, но в локальном кеше есть факты
      // для того же трека — показываем их сразу и не отправляем ручной сетевой запрос.
      if (cachedFacts.length > 0 && cached.title && data.title && cached.title === data.title) {
        content.innerHTML = cachedFacts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('');
        return;
      }
      // [FIX] Если есть служебный статус — показываем его, без подмены факта
      if (data.status && data.status.length > 0) {
        content.innerHTML = '<p class="no-facts">' + escapeHtmlFacts(data.status) + '</p>';
        return;
      }
      // [FIX] Ручной запрос факта по клику на логотип/обложку
      sendWS('trackfactsrequest=1');
      content.innerHTML = '<p class="no-facts">⏳ Запрос факта...</p>';
      // После ручного запроса не оставляем "вечное ожидание":
      // опрашиваем API до готовности факта/ошибки с ограничением по времени.
      pollFactsDialogUntilReady(15000);
      return;
    }
    
    // Если факты пришли от API — сразу обновляем глобальный кеш.
    setTrackFactsCache(data.title || '', facts);
    content.innerHTML = facts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('');
  } catch (e) {
    content.innerHTML = '<p class="no-facts">Ошибка загрузки фактов</p>';
  }
}

function escapeHtmlFacts(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

// Функция рендера версии в футере WebUI.

function renderVersionLink() {
  // Получаем элемент футера, куда выводится версия.
  const versionEl = getId('version');
  if (!versionEl) return; // Если элемента нет, тихо выходим.

  // Инициализируем значения по умолчанию:

  let prefix = '';
  let ver = yoVersion;

  // Регулярное выражение:
  // ^(.*\s)  — любая строка с пробелом на конце (префикс),
  // (v[\d.]+)$ — часть, начинающаяся с "v" и далее цифры/точки до конца строки.
  const m = String(yoVersion).match(/^(.*\s)(v[\d.]+)$/);
  if (m) {
    prefix = m[1]; // 
    ver = m[2];    // 
  } else {
    // Если шаблон не подошёл, оставляем всё как есть в одном куске.
    prefix = '';
    ver = String(yoVersion);
  }

  // Сборка HTML:
  // - Перед версией остаётся разделитель " | " (как и было),

  // - номер версии заворачиваем в <a> с target="_blank".
  versionEl.innerHTML =
    ' | ' +
    prefix +
    '<a target="_blank" href="https://github.com/ldisco/yoradio_lstepMOD">' +
    ver +
    '</a>';
}

function continueLoading(mode){
  if(typeof mode === 'undefined') return;
  if(mode=="player"){
    const plmt = localStorage.getItem("playlistmod");
    if(plmt)
      playlistmod = plmt;
    else
      localStorage.setItem("playlistmod", playlistmod);
    
    const pathname = window.location.pathname;
    if(['/','/index.html'].includes(pathname)){
      document.title = `${yoTitle} - Player`;
      fetch(`player.html?${yoVersion}`).then(response => response.text()).then(player => { 
        getId('content').classList.add('idx');
        getId('content').innerHTML = player;
        markShellReady(pathname);
        if (typeof window.rebindCoverArtObservers === 'function') window.rebindCoverArtObservers();
        // Показываем UI сразу после разметки; logo.svg может подгружаться с задержкой — раньше из‑за этого висел спиннер.
        hideSpinner();
        audiopreview=getId('audiopreview');
        initClock();
        setTimeout(() => {
          document.addEventListener('click', function(e) {
            const dialog = getId('genredialog');
            const genreBtn = getId('genrebutton');
            
            if (dialog && dialog.style.display === 'block' && 
                !dialog.contains(e.target) && 
                !genreBtn.contains(e.target)) {
              dialog.style.display = 'none';
              genreBtn.classList.remove('active');
            }
            
            const factsDialog = getId('factsdialog');
            const logoEl = getId('logo');
            const coverEl = getId('cover-art-display');
            if (factsDialog && factsDialog.style.display === 'block' &&
                !factsDialog.contains(e.target) &&
                (!logoEl || !logoEl.contains(e.target)) &&
                (!coverEl || !coverEl.contains(e.target))) {
              closeFactsDialog();
            }
          });
          
          const logoEl = getId('logo');
          const coverEl = getId('cover-art-display');
          if (logoEl) {
            logoEl.addEventListener('click', function(e) {
              e.stopPropagation();
              toggleFactsDialog();
            });
          }
          if (coverEl) {
            coverEl.addEventListener('click', function(e) {
              e.stopPropagation();
              toggleFactsDialog();
            });
          }
        }, 100);
        fetch('logo.svg').then(response => response.text()).then(svg => { 
          getId('logo').innerHTML = svg;
        }).catch(function(){});
     
        renderVersionLink();
        document.querySelectorAll('input[type="range"]').forEach(sl => { fillSlider(sl); });
        sendWS('getindex=1');
        sendWS('gettrackfacts=1'); // [FIX] Запрашиваем состояние TrackFacts для player page
      }).catch(function(){ hideSpinner(); });
    }
    if(pathname=='/settings.html'){
      document.title = `${yoTitle} - Settings`;
      fetchShellHtml(`options.html?${yoVersion}`).then(options => {
        getId('content').innerHTML = options; 
        markShellReady(pathname);
        hideSpinner();
        fetch('logo.svg').then(response => response.text()).then(svg => { 
          getId('logo').innerHTML = svg;
        }).catch(function(){});
        renderVersionLink();
        document.querySelectorAll('input[type="range"]').forEach(sl => { fillSlider(sl); });
        sendWS('getsystem=1');
        sendWS('getscreen=1');
        sendWS('gettimezone=1');
        sendWS('getweather=1');
        sendWS('getcontrols=1');
        sendWS('gettrackfacts=1');
        // Навешиваем обработчик смены провайдера для обновления описания ключа
        setTimeout(() => {
          let sel = getId('tfprovider');
          if (sel) {
            sel.onchange = updateKeyDesc;
            updateKeyDesc(false); // [FIX] НЕ отправляем на сервер при инициализации — значение ещё не загружено с ESP32!
          }
        }, 100);
        getWiFi(`http://${hostname}/data/wifi.csv`+"?"+new Date().getTime());
        sendWS('getactive=1');
        settingsActReceived = false;
        settingsActRetryCount = 0;
        scheduleSettingsActRetry();
        classEach("reset", function(el){ el.innerHTML='<svg viewBox="0 0 16 16" class="fill"><path d="M8 3v5a36.973 36.973 0 0 1-2.324-1.166A44.09 44.09 0 0 1 3.417 5.5a52.149 52.149 0 0 1 2.26-1.32A43.18 43.18 0 0 1 8 3z"/><path d="M7 5v1h4.5C12.894 6 14 7.106 14 8.5S12.894 11 11.5 11H1v1h10.5c1.93 0 3.5-1.57 3.5-3.5S13.43 5 11.5 5h-4z"/></svg>'; });
        // Загружаем информацию о файловой системе в TOOLS
        loadFsInfo();
      }).catch(function(){ hideSpinner(); });
    }
    if(pathname=='/update.html'){
      document.title = `${yoTitle} - Update`;
      fetch(`updform.html?${yoVersion}`).then(response => response.text()).then(updform => {
        getId('content').classList.add('upd');
        getId('content').innerHTML = updform; 
        markShellReady(pathname);
        hideSpinner();
        fetch('logo.svg').then(response => response.text()).then(svg => { 
          getId('logo').innerHTML = svg;
        }).catch(function(){});

        renderVersionLink();
      }).catch(function(){ hideSpinner(); });
    }
    if(pathname=='/ir.html'){
      document.title = `${yoTitle} - IR Recorder`;
      fetch(`irrecord.html?${yoVersion}`).then(response => response.text()).then(ircontent => {
        getId('content').innerHTML = ircontent;
        markShellReady(pathname);
        hideSpinner();
        loadCSS(`ir.css?${yoVersion}`);
        loadJS(`ir.js?${yoVersion}`, () => {
          fetch('logo.svg').then(response => response.text()).then(svg => { 
            getId('logo').innerHTML = svg;
            initControls();
          }).catch(function(){});
        });

        renderVersionLink();
      }).catch(function(){ hideSpinner(); });
    }
  }else{ // AP mode
    fetchShellHtml(`options.html?${yoVersion}`).then(options => {
      getId('content').innerHTML = options; 
      markShellReady(window.location.pathname);
      hideSpinner();
      fetch('logo.svg').then(response => response.text()).then(svg => { 
        getId('logo').innerHTML = svg;
      }).catch(function(){});

      renderVersionLink();
      getWiFi(`http://${hostname}/data/wifi.csv`+"?"+new Date().getTime());
      sendWS('getactive=1');
      if(window.location.pathname=='/settings.html'){
        settingsActReceived = false;
        settingsActRetryCount = 0;
        scheduleSettingsActRetry();
      }
    }).catch(function(){ hideSpinner(); });
  }
  if(clickUiAttached) return;
  clickUiAttached = true;
  document.body.addEventListener('click', (event) => {
  let target = event.target.closest('div, span, li');
  if(!target) return;
  if(target.classList.contains("knob")) target = target.parentElement;
  if(target.classList.contains("snfknob")) target = target.parentElement;
  if(target.parentElement.classList.contains("play")){ playItem(target.parentElement); return; }
  if(target.classList.contains("navitem")) { getId(target.dataset.target).scrollIntoView({ behavior: 'smooth' }); return; }
  if(target.classList.contains("reset")) { sendWS("reset="+target.dataset.name, true); return; }
  if(target.classList.contains("done")) { window.location.href=`http://${hostname}/`; return; }
  let command = target.dataset.command;
  if (command){
    if(target.classList.contains("local")){
      switch(command){
        case "toggle": 
          toggleTarget(target, target.dataset.target); 
          break;
        case "settings": window.location.href=`http://${hostname}/settings.html`; break;
          case "plimport": break;
          case "plexport": window.open(`http://${hostname}/data/playlist.csv`); break;
          case "pladd": plAdd(); break;
          case "pldel": plRemove(); break;
          case "plsubmit": submitPlaylist(); break;
          case "fwupdate": window.location.href=`http://${hostname}/update.html`; break;
          case "webboard": window.location.href=`http://${hostname}/webboard`; break;
          case "setupir": window.location.href=`http://${hostname}/ir.html`; break;
          case "applyweather":
            let key=getId("wkey").value;
            if(key!=""){
              sendWS("lat="+getId("wlat").value);
              sendWS("lon="+getId("wlon").value);
              sendWS("key="+key);
            }
            target.innerHTML='✓ Applied';
            setTimeout(function(){ target.innerHTML='Apply'; }, 2000);
            break;
          case "applytrackfacts":
            // Сохраняем ВСЕ настройки TrackFacts (включая провайдер!)
            let tfenabled = getId("tfen").classList.contains("checked") ? 1 : 0;
            let tfprov = getId("tfprovider").value;  // [FIX] Провайдер тоже нужно отправить!
            let tflang = getId("tflang").value;
            let tfcount = getId("tfcount").value;
            let tfkey = getId("tfkey").value;
            // Отправляем все настройки — провайдер ПЕРВЫМ, чтобы он был сохранён до ключа
            sendWS("trackfactsprovider="+tfprov);
            sendWS("trackfactsenabled="+tfenabled);
            sendWS("trackfactslang="+tflang);
            sendWS("trackfactscount="+tfcount);
            if(tfkey!=""){
              sendWS("applytrackfacts="+tfkey, true);
            }
            target.innerHTML='✓ Applied';
            setTimeout(function(){ target.innerHTML='Apply'; }, 2000);
            break;
          case "applytz": applyTZ(); target.innerHTML='✓ Applied'; setTimeout(function(){ target.innerHTML='Apply'; }, 2000); break;
          case "wifiexport": window.open(`http://${hostname}/data/wifi.csv`+"?"+new Date().getTime()); break;
          case "wifiupload": submitWiFi(); break;
          case "reboot": sendWS("reboot=1", true); rebootSystem('Rebooting...'); break;
          case "format": sendWS("format=1", true); rebootSystem('Format LittleFS. Rebooting...'); break;
          case "reset":  sendWS("reset=1", true);  rebootSystem('Reset settings. Rebooting...'); break;
          case "snuffle": toggleSnuffle(); break;
          case "rebootmdns": sendWS(`mdnsname=${getId('mdns').value}`, true); sendWS("rebootmdns=1", true); break;
          case "sleeptimerstep": sendWS("sleeptimerstep=1", true); break;
          case "filemanager": toggleFileManager(); event.stopPropagation(); break;
          
          case "genre":
          toggleGenreDialog();
          event.stopPropagation();
          break;
        case "toggleFavorites": toggleFavoritesFilter(); break;
        /* Поиск по плейлисту */
        case "searchplaylist": toggleSearchDialog(); break;
        case "searchclose": closeSearchDialog(); break;
          
        default: break;
      }
    }else{
        if(target.classList.contains("checkbox")) checkboxClick(target, command);
        if(target.classList.contains("cmdbutton")) { sendWS(`${command}=1`, true); }
        if(target.classList.contains("modeitem")) changeMode(target);
        if(target.hasClass("pleplay")) playPreview(target.parentElement);
        if(target.classList.contains("play")){
          const item = target.attr('attr-id');
          setCurrentItem(item)
          sendWS(`${command}=${item}`, true);
        }
      }
      event.preventDefault(); event.stopPropagation();
    }
  });
  document.body.addEventListener('input', (event) => {
    let target = event.target;
    let command = target.dataset.command;
    if (!command) { command = target.parentElement.dataset.command; target = target.parentElement; }
    if (command) {
      if(target.classList.contains("local")){
        switch(command){
          case "plselect": let ch=target.checked; classEach('plcb', function(el){ el.checked=ch; });
          default: break;
        };
        return;
      }
      if(target.type==='range') sliderInput(target, command);  //<-- range
      else sendWS(`${command}=${target.value}`);       //<-- other
      event.preventDefault(); event.stopPropagation();
    }
  });
  document.body.addEventListener('mousewheel', (event) => {
    const target = event.target;
    if(target.type==='range'){
      const command = target.dataset.command;
      target.valueAsNumber += event.deltaY>0?-1:1;
      if (command) {
        sliderInput(target, command);
      }
    }
  });
}
/** UPDATE **/
var uploadWithError = false;
function doUpdate(el) {
  let binfile = getId('binfile').files[0];
  if(binfile){
    getId('updateform').attr('class','hidden');
    getId("updateprogress").value = 0;
    getId('updateprogress').hidden=false;
    getId('update_cancel_button').hidden=true;
    var formData = new FormData();
    // Server expects the exact value "LittleFS" for filesystem uploads
    formData.append("updatetarget", getId('uploadtype1').checked?"firmware":"LittleFS");
    formData.append("update", binfile);
    var xhr = new XMLHttpRequest();
    uploadWithError = false;
    xhr.onreadystatechange = function() {
      if (xhr.readyState == XMLHttpRequest.DONE) {
        if(xhr.responseText!="OK"){
          getId("uploadstatus").innerHTML = xhr.responseText;
          uploadWithError=true;
        }
      }
    }
    xhr.upload.addEventListener("progress", progressHandler, false);
    xhr.addEventListener("load", completeHandler, false);
    xhr.addEventListener("error", errorHandler, false);
    xhr.addEventListener("abort", abortHandler, false);
    xhr.open("POST",`http://${hostname}/update`,true);
    xhr.send(formData);
  }else{
    alert('Choose something first');
  }
}
function progressHandler(event) {
  var percent = (event.loaded / event.total) * 100;
  getId("uploadstatus").innerHTML = Math.round(percent) + "%&nbsp;&nbsp;uploaded&nbsp;&nbsp;|&nbsp;&nbsp;please wait...";
  getId("updateprogress").value = Math.round(percent);
  if (percent >= 100) {
    getId("uploadstatus").innerHTML = "Please wait, writing file to filesystem";
  }
}
var tickcount=0;
function rebootingProgress(){
  getId("updateprogress").value = Math.round(tickcount/7);
  tickcount+=14;
  if(tickcount>700){
    location.href=`http://${hostname}/`;
  }else{
    setTimeout(rebootingProgress, 200);
  }
}
function completeHandler(event) {
  if(uploadWithError) return;
  getId("uploadstatus").innerHTML = "Upload Complete, rebooting...";
  rebootingProgress();
}
function errorHandler(event) {
  getId('updateform').attr('class','');
  getId('updateprogress').hidden=true;
  getId("updateprogress").value = 0;
  getId("status").innerHTML = "Upload Failed";
}
function abortHandler(event) {
  getId('updateform').attr('class','');
  getId('updateprogress').hidden=true;
  getId("updateprogress").value = 0;
  getId("status").innerHTML = "inUpload Aborted";
}
/** UPDATE **/

function updateKeyDesc(send = true){
  let p = getId("tfprovider");
  let k = getId("tfkey");
  if (!p || !k) return;
  
  let val = p.value;
  let row = k.closest('.flex-row');
  let span = k.previousElementSibling;
  
  if (!row || !span) return;

  // Отправляем команду на радио только если это ручное изменение (не инициализация по websocket)
  if (send) sendWS(`trackfactsprovider=${val}`);

  // Провайдеры: Gemini=0, DeepSeek=1, iTunes=2, LastFM=3, MusicBrainz=4
  if (val == 2 || val == 4) { 
    row.classList.add("hidden");
  } else {
    row.classList.remove("hidden");
    if (val == 0) {
      span.innerHTML = 'google gemini api key [<a href="https://aistudio.google.com/" target="_blank">get free key</a>]';
      k.placeholder = "AIzaSyBxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    }
    if (val == 1) {
      span.innerHTML = 'deepseek api key [<a href="https://platform.deepseek.com/" target="_blank">get key</a>]';
      k.placeholder = "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    }
    if (val == 3) {
      span.innerHTML = 'last.fm api key [<a href="https://www.last.fm/api/account/create" target="_blank">get key</a>]';
      k.placeholder = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    }
  }
}

// ============================================================================
// [v0.4.2] FsInfo — загрузка информации о файловой системе в секцию Tools
// Вызывается сразу после того как options.html вставлен в DOM.
// ============================================================================
function loadFsInfo() {
  var el = document.getElementById('fsinfo');
  if (!el) return;
  fetch('/api/fs-info?t=' + Date.now())
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var info = "Total: " + d.totalMB + " MB | Used: " + d.usedMB + " MB | Free: " + d.freeMB + " MB";
      if (d.psramTotal && d.psramTotal > 0) {
        info += "<br>PSRAM: " + d.psramTotalMB + " MB | Used: " + d.psramUsedMB + " MB | Free: " + d.psramFreeMB + " MB";
      }
      el.innerHTML = info;
    })
    .catch(function(e) { console.error("FS Info error", e); });
}

// ============================================================================
// [v0.6.0] File Manager — модальный файловый менеджер с авторизацией
// ============================================================================
var _fmCurrentDir = '/';
var _fmOpen = false;
var _fmKey = '';

function fmAuthKey() {
  return _fmKey ? ('&key=' + encodeURIComponent(_fmKey)) : '';
}

function toggleFileManager() {
  var dlg = getId('fmdialog');
  var overlay = getId('fm-overlay');
  if (!dlg) return;
  if (!_fmOpen) {
    // Перемещаем диалог и оверлей в body для корректного position:fixed на мобильных
    if (dlg.parentNode !== document.body) {
      document.body.appendChild(dlg);
      if (overlay) document.body.appendChild(overlay);
    }
    // Если ключ ещё не введён — запрашиваем
    if (!_fmKey) {
      var pwd = prompt('File Manager password:');
      if (!pwd) return;
      fetch('/api/fs-auth?key=' + encodeURIComponent(pwd) + '&t=' + Date.now())
        .then(function(r) { return r.json(); })
        .then(function(d) {
          if (d.ok) {
            _fmKey = pwd;
            _fmOpen = true;
            dlg.style.display = 'flex';
            if (overlay) overlay.style.display = 'block';
            _fmCurrentDir = '/';
            fmLoadDir('/');
          } else {
            alert('Wrong password');
          }
        })
        .catch(function() { alert('Auth error'); });
      return;
    }
    _fmOpen = true;
    dlg.style.display = 'flex';
    if (overlay) overlay.style.display = 'block';
    _fmCurrentDir = '/';
    fmLoadDir('/');
  } else {
    _fmOpen = false;
    dlg.style.display = 'none';
    if (overlay) overlay.style.display = 'none';
  }
}

function fmLoadDir(dir) {
  _fmCurrentDir = dir;
  var list = getId('fm-filelist');
  var bc = getId('fm-breadcrumb');
  var status = getId('fm-status');
  if (!list) return;
  list.innerHTML = '<div class="fm-loading">Loading...</div>';
  if (status) status.innerHTML = '';

  // Хлебные крошки
  var parts = dir.split('/').filter(function(p) { return p.length > 0; });
  var bcHtml = '<span class="fm-bc-item" onclick="fmLoadDir(\'/\')">/ root</span>';
  var path = '';
  for (var i = 0; i < parts.length; i++) {
    path += '/' + parts[i];
    bcHtml += ' / <span class="fm-bc-item" onclick="fmLoadDir(\'' + path + '\')">' + parts[i] + '</span>';
  }
  if (bc) bc.innerHTML = bcHtml;

  fetch('/api/fs-list?dir=' + encodeURIComponent(dir) + fmAuthKey() + '&t=' + Date.now())
    .then(function(r) { return r.json(); })
    .then(function(files) {
      if (files.error) {
        if (files.error === 'auth') { _fmKey = ''; toggleFileManager(); return; }
        list.innerHTML = '<div class="fm-error">' + files.error + '</div>';
        return;
      }
      // Сортировка: папки первые, потом по имени
      files.sort(function(a, b) {
        if (a.isDir && !b.isDir) return -1;
        if (!a.isDir && b.isDir) return 1;
        return a.name.localeCompare(b.name);
      });
      var html = '';
      // Кнопка "вверх" если не корень
      if (dir !== '/') {
        var parent = dir.substring(0, dir.lastIndexOf('/')) || '/';
        html += '<div class="fm-item fm-dir" onclick="fmLoadDir(\'' + parent + '\')">';
        html += '<span class="fm-icon">&#x2B06;</span>';
        html += '<span class="fm-name">..</span>';
        html += '<span class="fm-size"></span>';
        html += '<span class="fm-actions"></span>';
        html += '</div>';
      }
      for (var i = 0; i < files.length; i++) {
        var f = files[i];
        var fullPath = (dir === '/' ? '/' : dir + '/') + f.name;
        html += '<div class="fm-item' + (f.isDir ? ' fm-dir' : '') + '">';
        if (f.isDir) {
          html += '<span class="fm-icon" onclick="fmLoadDir(\'' + fullPath + '\')">&#x1F4C1;</span>';
          html += '<span class="fm-name" onclick="fmLoadDir(\'' + fullPath + '\')">' + f.name + '</span>';
          html += '<span class="fm-size">DIR</span>';
        } else {
          html += '<span class="fm-icon">&#x1F4C4;</span>';
          html += '<span class="fm-name">' + f.name + '</span>';
          html += '<span class="fm-size">' + fmFormatSize(f.size) + '</span>';
        }
        html += '<span class="fm-actions">';
        if (!f.isDir) {
          html += '<span class="fm-btn" onclick="fmDownload(\'' + fullPath + '\')" title="Download">&#x2B07;</span>';
        }
        html += '<span class="fm-btn fm-btn-del" onclick="fmDelete(\'' + fullPath + '\',\'' + f.name + '\',' + (f.isDir?'true':'false') + ')" title="Delete">&#x2716;</span>';
        html += '</span></div>';
      }
      if (files.length === 0) {
        html += '<div class="fm-empty">Empty directory</div>';
      }
      list.innerHTML = html;
    })
    .catch(function(e) {
      list.innerHTML = '<div class="fm-error">Error: ' + e.message + '</div>';
    });
}

function fmFormatSize(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / 1048576).toFixed(1) + ' MB';
}

function fmDownload(path) {
  window.open('/api/fs-download?path=' + encodeURIComponent(path) + fmAuthKey());
}

function fmDelete(path, name, isDir) {
  if (!confirm('Delete ' + (isDir ? 'folder' : 'file') + ' "' + name + '"?')) return;
  var status = getId('fm-status');
  fetch('/api/fs-delete?path=' + encodeURIComponent(path) + fmAuthKey() + '&t=' + Date.now())
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.ok) {
        if (status) status.innerHTML = '<span style="color:#4a4">Deleted: ' + name + '</span>';
        fmLoadDir(_fmCurrentDir);
        loadFsInfo();
      } else {
        if (status) status.innerHTML = '<span style="color:#a44">' + (d.error || 'Delete failed') + '</span>';
      }
    })
    .catch(function(e) {
      if (status) status.innerHTML = '<span style="color:#a44">Error: ' + e.message + '</span>';
    });
}

function fmUpload() {
  var fileInput = getId('fm-file-input');
  var status = getId('fm-status');
  if (!fileInput || !fileInput.files.length) {
    if (status) status.innerHTML = '<span style="color:#a44">Select a file first</span>';
    return;
  }
  var file = fileInput.files[0];
  var formData = new FormData();
  formData.append('file', file);
  formData.append('dir', _fmCurrentDir);
  if (_fmKey) formData.append('key', _fmKey);
  if (status) status.innerHTML = '<span style="color:#666">Uploading ' + file.name + '...</span>';

  fetch('/api/fs-upload', { method: 'POST', body: formData })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.ok) {
        if (status) status.innerHTML = '<span style="color:#4a4">Uploaded: ' + file.name + '</span>';
        fileInput.value = '';
        fmLoadDir(_fmCurrentDir);
        loadFsInfo();
      } else {
        if (status) status.innerHTML = '<span style="color:#a44">Upload failed</span>';
      }
    })
    .catch(function(e) {
      if (status) status.innerHTML = '<span style="color:#a44">Error: ' + e.message + '</span>';
    });
}

// ============================================================================
// Функция инициализации и обновления часов
// Обновляет время каждую секунду в формате чч:мм
// Точка между часами и минутами мигает с периодом 2 секунды
// ============================================================================
function initClock() {
  function update() {
    const now = new Date();
    const h = String(now.getHours()).padStart(2, '0');
    const m = String(now.getMinutes()).padStart(2, '0');
    const eH = getId('clock-hours');
    const eM = getId('clock-minutes');
    if (eH) eH.innerText = h;
    if (eM) eM.innerText = m;
  }
  
  // Если блока с часами еще нет в верстке - добавляем его программно перед "powered by"
  if (!getId('clockdisplay')) {
    const clockHTML = `<span id="clockdisplay"><span id="clock-hours">00</span><span class="clock-dot">:</span><span id="clock-minutes">00</span></span>&nbsp;`;
    const copy = getId('copy');
    if (copy) {
      copy.insertAdjacentHTML('afterbegin', clockHTML);
    }
  }
  
  update();
  setInterval(update, 1000);
}

// ============================================================================
// [v0.4.4] TrackFacts - модуль для отображения фактов о композициях
// ============================================================================
(function() {
    'use strict';
    const CONFIG = {
        API_ENDPOINT: '/api/current-fact',
        FACT_SHOW_TIME: 15000,
        PAUSE_TIME: 15000,
        CHECK_INTERVAL: 4000,
        FETCH_DELAY: 2000
    };
    let lastTitle = '';
    let lastRawFacts = '';
    let currentFacts = [];
    let currentFactIndex = 0;
    let isShowingFact = false;
    let cycleTimer = null;
    function isPlayerPlaying() {
        const playerwrap = document.getElementById('playerwrap');
        return playerwrap && playerwrap.classList.contains('playing');
    }
    function getMetaElement() { return document.getElementById('meta'); }
    function resetAll() {
        if (cycleTimer) { clearTimeout(cycleTimer); cycleTimer = null; }
        isShowingFact = false;
        currentFacts = [];
        currentFactIndex = 0;
        lastRawFacts = '';
        // [FIX] При полном сбросе очищаем и общий кеш диалога, чтобы не показывать устаревшие данные.
        setTrackFactsCache('', []);
        const meta = getMetaElement();
        if (meta) {
            meta.classList.remove('track-fact');
            meta.style.fontSize = ''; meta.style.color = ''; meta.style.fontStyle = ''; meta.style.lineHeight = '';
            if (lastTitle) meta.textContent = lastTitle;
        }
    }
    window.resetTrackFacts = function() { lastTitle = ''; resetAll(); };
    function displayNext() {
        const meta = getMetaElement();
        if (!meta || !isPlayerPlaying()) { resetAll(); return; }
        // [FIX] Когда диалог фактов открыт, показываем трек вместо факта
        if (window.factsDialogOpen) {
            meta.classList.remove('track-fact');
            meta.style.fontSize = ''; meta.style.color = ''; meta.style.fontStyle = ''; meta.style.lineHeight = '';
            if (lastTitle) meta.textContent = lastTitle;
            cycleTimer = setTimeout(displayNext, 2000);
            return;
        }
        if (!isShowingFact && currentFacts.length > 0) {
            const fact = currentFacts[currentFactIndex];
            if (!fact || fact.trim().length === 0) {
                currentFactIndex = (currentFactIndex + 1) % currentFacts.length;
                cycleTimer = setTimeout(displayNext, 1000);
                return;
            }
            meta.classList.add('track-fact');
            meta.style.fontSize = '1.2em'; meta.style.fontStyle = 'italic'; meta.style.lineHeight = '1.4';
            meta.textContent = '💡 ' + fact;
            isShowingFact = true;
            currentFactIndex = (currentFactIndex + 1) % currentFacts.length;
            cycleTimer = setTimeout(displayNext, CONFIG.FACT_SHOW_TIME);
        } else {
            meta.classList.remove('track-fact');
            meta.style.fontSize = ''; meta.style.color = ''; meta.style.fontStyle = ''; meta.style.lineHeight = '';
            meta.textContent = lastTitle;
            isShowingFact = false;
            cycleTimer = setTimeout(displayNext, CONFIG.PAUSE_TIME);
        }
    }
    async function checkAndDisplayFact() {
        if (!isPlayerPlaying()) { resetAll(); return; }
        try {
            const response = await fetch(CONFIG.API_ENDPOINT + '?t=' + Date.now());
            if (!response.ok) return;
            const data = await response.json();
            if (!data || !data.title) return;
            if (data.title !== lastTitle) {
                lastTitle = data.title;
                resetAll();
                const facts = (data.facts || []).filter(function(f) { return f && f.trim().length > 0; });
                lastRawFacts = facts.join('###');
                if (facts.length > 0) {
                    currentFacts = facts;
                    // [FIX] Синхронизируем глобальный кеш сразу после получения фактов от API.
                    setTrackFactsCache(data.title || '', facts);
                    currentFactIndex = 0;
                    cycleTimer = setTimeout(displayNext, CONFIG.FETCH_DELAY);
                }
            } else {
                const facts = (data.facts || []).filter(function(f) { return f && f.trim().length > 0; });
                const newRawFacts = facts.join('###');
                if (newRawFacts !== lastRawFacts && newRawFacts.length > 0) {
                    lastRawFacts = newRawFacts;
                    currentFacts = facts;
                    // [FIX] Обновляем глобальный кеш при изменении списка фактов на том же треке.
                    setTrackFactsCache(data.title || '', facts);
                    if (!cycleTimer && !isShowingFact) displayNext();
                }
                if (facts.length === 0 && !data.pending) resetAll();
            }
            const meta = getMetaElement();
            if (meta && !isShowingFact && currentFacts.length === 0 && data.pending) {
                meta.textContent = lastTitle + ' ⏳';
            }
        } catch (error) { console.error('[TrackFacts] Error:', error); }
    }
    function init() {
        setInterval(checkAndDisplayFact, CONFIG.CHECK_INTERVAL);
        checkAndDisplayFact();
    }
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();
})();

// Обложки: только cover.js (подключается выше), без дубликата — иначе двойной poll и «мёртвый» observer.

// ============================================================================
// Drag-n-Drop для редактора плейлиста
// ============================================================================
(function() {
    let dragged, id, index, indexDrop, list;
    document.addEventListener("dragstart", ({target}) => {
        if (!target.parentNode || !target.parentNode.parentNode) return;
        dragged = target.parentNode;
        id = target.parentNode.id;
        list = target.parentNode.parentNode.children;
        for(let i = 0; i < list.length; i += 1) if(list[i] === dragged) index = i;
    });
    document.addEventListener("dragover", (event) => event.preventDefault());
    document.addEventListener("drop", ({target}) => {
        if(target.parentNode.className == "pleitem" && target.parentNode.id !== id) {
            dragged.remove( dragged );
            for(let i = 0; i < list.length; i += 1) if(list[i] === target.parentNode) indexDrop = i;
            if(index > indexDrop) target.parentNode.before( dragged ); else target.parentNode.after( dragged );
            let items=document.getElementById('pleditorcontent').getElementsByTagName('li');
            for (let i = 0; i <= items.length-1; i++) items[i].getElementsByTagName('span')[0].innerText=("00"+(i+1)).slice(-3);
        }
    });
})();
