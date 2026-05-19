var hostname = window.location.hostname;
var modesd = false;
// Прошивка с DLNA шлёт playermode === 'modedlna' при PL_SRC_DLNA (остаёмся в «веб»-ветке плеера, не SD).
var modedlna = false;
var bigplaylist = false;
const query = window.location.search;
const params = new URLSearchParams(query);
const yoTitle = 'Buzig';
let audiopreview=null;
if(params.size>0){
  if(params.has('host')) hostname=params.get('host');
}
var websocket;
var wserrcnt = 0;
var wstimeout, pongtimeout;
// Watchdog переподключения: страхует сценарии после reboot, когда браузер не всегда корректно сообщает onclose.
var wsWatchdogTimer = null;
var wsConnectStartedAt = 0;
// [FIX][SD+WEB] Таймер клиентского heartbeat для мобильных браузеров.
// Нужен, чтобы сокет не считался "пассивным" и не уходил в rx timeout на стороне ESP.
var wsPingTimer = null;
var wsResyncTimer = null;
var wsDisconnectToastTimer = null;
// Оболочка страницы (player/options/…) подгружается fetch'ами — нельзя считать её
// загруженной до успешного innerHTML, иначе при быстром WS-reconnect второй onOpen
// пропускает continueLoading() и UI остаётся пустым/старым до F5.
var shellReady = false;
var shellPathname = '';
var clickUiAttached = false;
function markShellReady(pathname){

  shellReady = true;
  shellPathname = pathname || '';
}
var currentItem = 0;
var sdIndexingActive = false; // [FIX] Флаг индексации SD — блокирует обновление nameset/meta
// Флаг первого получения playermode: нужен, чтобы выполнить инициализацию один раз,
// но не повторять тяжёлый путь (перезагрузка плейлиста/обложки) на каждом повторном кадре.
var playerModeBootstrapped = false;
var trackFactsEnabled = false; // [FIX] Глобальный флаг — включены ли TrackFacts (обновляется из WS)
var sleepTimerState = { m: 0, left: 0, active: 0, alloff: 0, supported: 0 };
var playlistmod = new Date().getTime();
// Авто-повтор загрузки плейлиста после старта/ребута:
// если /api/playlist в первые секунды вернул пустой ответ или сеть дала сбой,
// делаем несколько коротких повторов.
var playlistRetryTimer = null;
var playlistRetryCount = 0;
var PLAYLIST_RETRY_MAX = 8;
var PLAYLIST_RETRY_DELAY_MS = 900;
// Ожидание появления #playlist в DOM (раньше пришёл WS playermode, чем вставился player.html) — отдельный таймер,
// не тратит счётчик PLAYLIST_RETRY_MAX и не конкурирует с «пустым ответом» /api/playlist.
var playlistDomWaitTimer = null;
var playlistDomWaitCount = 0;
var PLAYLIST_DOM_WAIT_MAX = 120;
var PLAYLIST_DOM_WAIT_MS = 80;
var lastNamesetText = '';
// Троттлинг обновлений meta/nameset по отдельности: не чаще раз в 400 мс, чтобы плейлист не дёргался
var lastMetaUpdate = 0, lastNamesetUpdate = 0;
var META_NAMESET_THROTTLE_MS = 400;
// WebSocket: две очереди — «immediate» (кнопки плеера и т.п.) не должны ждать сотни get*/настроек при CONNECTING/reconnect.
var wsSendQueueHigh = [];
var wsSendQueueLow = [];
var wsSendTimer = null;
var WS_SEND_INTERVAL = 80; // ms between sends
// Антиспам для уведомлений состояния WebSocket.
var lastWsStatusToastAt = 0;
var wsDisconnectToastShown = false;

// [FIX][SETTINGS] После /settings.html пункты меню ждут WS {"act":[...]}. Усиливаем: act и с payload в одном пакете,
// getactive вне очереди (immediate), двойной rAF перед первым запросом, больше ретраев.
var settingsActReceived = false;
var settingsActRetryCount = 0;
var settingsActRetryTimer = null;
var SETTINGS_ACT_RETRY_MAX = 12;
var SETTINGS_ACT_RETRY_MS = 550;
function settingsNavigationNeedsUnlock(){
  var nav = getId('navigation');
  if(!nav) return true;
  return !nav.querySelector('.navitem:not(.hidden)');
}
function applySettingsActFromServer(act){
  if(typeof act === 'undefined' || act === null || !act.forEach) return;
  act.forEach(function(showclass){
    classEach(showclass, function(el) { el.classList.remove("hidden"); });
  });
  if(window.location.pathname === '/settings.html'){
    settingsActReceived = true;
    clearTimeout(settingsActRetryTimer);
    settingsActRetryTimer = null;
  }
}
function scheduleSettingsActRetry(){
  clearTimeout(settingsActRetryTimer);
  settingsActRetryTimer = setTimeout(function(){
    if(window.location.pathname!='/settings.html') return;
    if(!settingsNavigationNeedsUnlock()) return;
    if(settingsActRetryCount >= SETTINGS_ACT_RETRY_MAX) return;
    settingsActRetryCount++;
    sendWS('getactive=1', true);
    scheduleSettingsActRetry();
  }, SETTINGS_ACT_RETRY_MS);
}
function bootstrapSettingsPageWsQueries(){
  if(window.location.pathname !== '/settings.html') return;
  settingsActReceived = false;
  settingsActRetryCount = 0;
  clearTimeout(settingsActRetryTimer);
  settingsActRetryTimer = null;
  function kick(){
    sendWS('getactive=1', true);
    // Все get* — immediate: иначе WS_SEND_INTERVAL мс × N в очереди + конкурирующий трафик → первый ответ приходит,
    // а при ошибке/пропуске в onMessage UI остаётся в дефолтах до ручного F5.
    sendWS('getsystem=1', true);
    sendWS('getscreen=1', true);
    sendWS('gettimezone=1', true);
    sendWS('getweather=1', true);
    sendWS('getcontrols=1', true);
    sendWS('gettrackfacts=1', true);
    scheduleSettingsActRetry();
  }
  if(window.requestAnimationFrame){
    requestAnimationFrame(function(){ requestAnimationFrame(kick); });
  }else{
    setTimeout(kick, 0);
  }
  setTimeout(function(){
    if(window.location.pathname !== '/settings.html') return;
    if(!settingsNavigationNeedsUnlock()) return;
    sendWS('getactive=1', true);
  }, 320);
  // Повтор полей настроек после стабилизации DOM/сокета (тот же сценарий, что лечит «всё OFF до F5»).
  setTimeout(function(){
    if(window.location.pathname !== '/settings.html') return;
    if(!getId('settingscontent')) return;
    sendWS('getsystem=1', true);
    sendWS('getscreen=1', true);
    sendWS('gettimezone=1', true);
    sendWS('getweather=1', true);
    sendWS('getcontrols=1', true);
    sendWS('gettrackfacts=1', true);
  }, 420);
  // Поздний добор состояния для "медленного старта" после reboot/подачи питания:
  // иногда плагины (SleepTimer) и часть системных флагов готовы не сразу, поэтому
  // ранние getsystem (0..420 мс) приходят без нужных полей и кнопки остаются hidden до F5.
  // Даём ещё один короткий запрос после стабилизации сервисов ESP и DOM.
  setTimeout(function(){
    // Страхуемся от ухода пользователя на другую страницу.
    if(window.location.pathname !== '/settings.html') return;
    // Если контейнер настроек уже не в DOM (переход/перерисовка), запрос пропускаем.
    if(!getId('settingscontent')) return;
    // Повторяем getactive, чтобы гарантированно восстановить видимость пунктов меню настройки.
    sendWS('getactive=1', true);
    // Повторяем getsystem: здесь приходят webcpu/wci и триггерится sleep.pushState на сервере.
    sendWS('getsystem=1', true);
  }, 1800);
}
// Первый заход в настройки: если меню осталось скрытым (act не пришёл до отрисовки), повторим при возврате на вкладку.
document.addEventListener('visibilitychange', function(){
  if(document.visibilityState !== 'visible') return;
  if(window.location.pathname !== '/settings.html') return;
  var nav = getId('navigation');
  if(!nav) return;
  if(!settingsNavigationNeedsUnlock()) return;
  settingsActReceived = false;
  settingsActRetryCount = 0;
  sendWS('getactive=1', true);
  scheduleSettingsActRetry();
});
function enqueueWS(msg, highPriority){
  try{ msg = String(msg); }catch(e){ msg = ''+msg; }
  // Пока сокет не OPEN, low-сообщения только копятся (раз в WS_SEND_INTERVAL мс никто не shift'ит) —
  // после паузы Wi‑Fi/фона на телефоне очередь из сотен volume=* разбирается минутами.
  if(!highPriority && (!websocket || websocket.readyState !== WebSocket.OPEN)) return;
  if(highPriority) wsSendQueueHigh.push(msg);
  else wsSendQueueLow.push(msg);
  if(!wsSendTimer) wsSendTimer = setTimeout(processWSSendQueue, WS_SEND_INTERVAL);
}
function processWSSendQueue(){
  var pending = wsSendQueueHigh.length + wsSendQueueLow.length;
  if(!pending){ clearTimeout(wsSendTimer); wsSendTimer = null; return; }
  if(websocket && websocket.readyState===WebSocket.OPEN){
    var msg = wsSendQueueHigh.length ? wsSendQueueHigh.shift() : wsSendQueueLow.shift();
    try{ websocket.send(msg); }catch(e){ /* ignore */ }
  }
  wsSendTimer = setTimeout(processWSSendQueue, WS_SEND_INTERVAL);
}
function sendWS(msg, immediate){
  var urgent = immediate === true;
  if(urgent && websocket && websocket.readyState===WebSocket.OPEN){
    try{ websocket.send(String(msg)); }catch(e){}
    return;
  }
  enqueueWS(msg, urgent);
}

// === COVER ART LOADER (один раз за сессию — повторная вставка давала второй setInterval / observer) ===
(function(){
  if(window.__coverScriptInjected) return;
  window.__coverScriptInjected=true;
  var s=document.createElement('script');s.src='/cover.js?_='+encodeURIComponent(String(typeof yoVersion!=='undefined'?yoVersion:'1'));document.head.appendChild(s);
})();
// === END COVER ART ===

// === TRACK FACTS LOADER ===
(function(){
  if(window.__factsScriptInjected) return;
  window.__factsScriptInjected=true;
  var s=document.createElement('script');s.src='/facts.js?_='+encodeURIComponent(String(typeof yoVersion!=='undefined'?yoVersion:'1'));document.head.appendChild(s);
})();
// === END TRACK FACTS ===

window.addEventListener('load', onLoad);
// BFCache (назад/вперёд в браузере): вкладка восстанавливается без полной перезагрузки, WebSocket «мёртв»,
// а shellReady остаётся true — onOpen пропускает continueLoading → пустой или устаревший UI до серии F5.
window.addEventListener('pageshow', function(ev){
  if(!ev.persisted) return;
  try{ if(websocket && websocket.readyState === WebSocket.OPEN) websocket.close(); }catch(e2){}
  shellReady = false;
  shellPathname = '';
  playerModeBootstrapped = false;
  initWebSocket();
});

function loadCSS(href){ const link = document.createElement("link"); link.rel = "stylesheet"; link.href = href; document.head.appendChild(link); }
function loadJS(src, callback){ const script = document.createElement("script"); script.src = src; script.type = "text/javascript"; script.async = true; script.onload = callback; document.head.appendChild(script); }
// Подгрузка фрагментов UI: при занятости ESP (SSL/аудио) fetch может «висеть» — снимаем спиннер по таймауту.
function fetchShellHtml(url, timeoutMs){
  // Возвращаем безопасный таймаут по умолчанию (20 сек), чтобы не срывать загрузку
  // на слабом Wi‑Fi/в момент пиковых задач ESP.
  timeoutMs = (typeof timeoutMs === 'number' && timeoutMs > 0) ? timeoutMs : 20000;
  return Promise.race([
    fetch(url).then(function(r){ return r.text(); }),
    new Promise(function(_, reject){
      setTimeout(function(){ reject(new Error('shell fetch timeout')); }, timeoutMs);
    })
  ]);
}

// Возвращает яркий цвет полосы буфера по порогам заполнения.
// Пороговая логика:
// 0-15%   -> ярко-красный,
// 15-50%  -> ярко-жёлтый,
// 50-100% -> ярко-зелёный.
function getHeapColorByPercent(percent){
  // Защищаемся от NaN/undefined, чтобы UI не получал некорректный цвет.
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

// Цвет полоски загрузки CPU в WebUI:
// 0–55% зелёный, 56–90% жёлтый, 91–100% красный.
function getCpuBarColorByPercent(percent){
  var p = Number(percent);
  if (!Number.isFinite(p)) p = 0;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  if (p <= 55) return '#39d964';
  if (p <= 90) return '#f2cb2f';
  return '#ff3f2f';
}

function initWebSocket(){
  clearTimeout(wstimeout);
  wstimeout = null;
  // [FIX][SD+WEB] Перед новым подключением останавливаем старый heartbeat,
  // чтобы не оставить несколько параллельных таймеров после reconnect.
  stopWsHeartbeat();
  
  // [FIX] Улучшенная логика переподключения с проверкой состояния сети
  if (typeof navigator !== 'undefined' && !navigator.onLine) {
    console.log('Network is offline, delaying WebSocket connection...');
    wstimeout = setTimeout(initWebSocket, 2000);
    return;
  }
  
  console.log('Trying to open a WebSocket connection...');
  try {
    wsConnectStartedAt = Date.now();
    websocket = new WebSocket(`ws://${hostname}/ws`);
    websocket.onopen    = onOpen;
    websocket.onclose   = onClose;
    websocket.onmessage = onMessage;
    websocket.onerror   = function(error) {
      console.log('WebSocket error:', error);
      // При ошибке сразу закрываем соединение для чистого переподключения
      if (websocket) websocket.close();
    };
  } catch (error) {
    console.log('WebSocket creation failed:', error);
    // При ошибке создания сокета пробуем переподключиться через 1 секунду
    wstimeout = setTimeout(initWebSocket, 1000);
  }
}
function startWsWatchdog(){
  // Запускаем только один интервал на сессию, иначе появятся дублирующие reconnect-попытки.
  if(wsWatchdogTimer) return;
  wsWatchdogTimer = setInterval(function(){
    // Если уже запланирован reconnect через backoff-таймер — не вмешиваемся.
    if(wstimeout) return;
    // Отсутствует сокет после reboot/пробуждения вкладки — мягко инициируем новое подключение.
    if(!websocket){
      initWebSocket();
      return;
    }
    // Закрытый сокет без onclose-ветки — пробуем открыть заново.
    if(websocket.readyState === WebSocket.CLOSED){
      initWebSocket();
      return;
    }
    // Подключение "залипло" слишком долго — закрываем и даём обычной логике пересоздать сокет.
    if(websocket.readyState === WebSocket.CONNECTING){
      var connectAgeMs = Date.now() - (wsConnectStartedAt || 0);
      if(connectAgeMs > 12000){
        try{ websocket.close(); }catch(e){}
      }
    }
  }, 3000);
}
function onLoad(event) {
  // Стоковое поведение: единственное действие при загрузке страницы — открыть WS.
  // Никаких параллельных таймеров автоповтора загрузки shell — это создавало гонки
  // (continueLoading вызывался дважды и UI мерцал/не успевал прорисовываться).
  initWebSocket();
  // Watchdog нужен только чтобы поднять WS, если onclose по какой-то причине не пришёл
  // (пробуждение вкладки, кратковременный пропуск событий браузером).
  startWsWatchdog();
}
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

// [FIX] Сохранение состояния UI при отключении WebSocket
function preserveUiStateOnDisconnect() {
  // Сохраняем текущее состояние кнопки воспроизведения
  var playButton = getId('play');
  if (playButton) {
    playButton.setAttribute('data-last-state', playButton.classList.contains('playing') ? 'playing' : 'paused');
  }
  
  // Сохраняем текущие метаданные и статус станции
  var metaElement = getId('meta');
  var stationElement = getId('station');
  if (metaElement && metaElement.innerText && !isSystemMetaValue(metaElement.innerText)) {
    metaElement.setAttribute('data-last-value', metaElement.innerText);
  }
  if (stationElement && stationElement.innerText) {
    stationElement.setAttribute('data-last-value', stationElement.innerText);
  }
}

// [FIX] Восстановление состояния UI при повторном подключении
function restoreUiStateOnReconnect() {
  var playButton = getId('play');
  if (playButton && playButton.getAttribute('data-last-state')) {
    var lastState = playButton.getAttribute('data-last-state');
    playButton.classList.toggle('playing', lastState === 'playing');
    playButton.classList.toggle('paused', lastState === 'paused');
  }
  
  // Восстанавливаем метаданные только если текущие значения системные или пустые
  var metaElement = getId('meta');
  var stationElement = getId('station');
  if (metaElement && metaElement.getAttribute('data-last-value') && 
      (isSystemMetaValue(metaElement.innerText) || !metaElement.innerText.trim())) {
    metaElement.innerText = metaElement.getAttribute('data-last-value');
  }
  if (stationElement && stationElement.getAttribute('data-last-value') && 
      (!stationElement.innerText.trim())) {
    stationElement.innerText = stationElement.getAttribute('data-last-value');
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
function notifyWsStatusToast(message, isError, minIntervalMs){
  var now = Date.now();
  var minMs = (typeof minIntervalMs === 'number' && minIntervalMs > 0) ? minIntervalMs : 3000;
  if(now - lastWsStatusToastAt < minMs) return;
  lastWsStatusToastAt = now;
  if(typeof window.showToast === 'function'){
    window.showToast(message, !!isError);
  }
}
function onOpen(event) {
  console.log('Connection opened');
  if(wsDisconnectToastTimer){
    clearTimeout(wsDisconnectToastTimer);
    wsDisconnectToastTimer = null;
  }
  // Если в момент открытия #meta пустой, сразу показываем служебный статус.
  // Это исключает раздражающую "пустоту" до прихода первого TITLE.
  var metaOnOpen = getId('meta');
  if(metaOnOpen && !String(metaOnOpen.innerText || '').trim()){
    metaOnOpen.innerText = '[соединение]';
  }
  // Если до этого было отключение, явно сообщаем пользователю о восстановлении.
  if(wsDisconnectToastShown){
    notifyWsStatusToast('Связь с устройством восстановлена.', false, 2000);
    wsDisconnectToastShown = false;
  }
  pingUp();
  // [FIX][SD+WEB] Запускаем клиентский heartbeat только после успешного открытия сокета.
  startWsHeartbeat();
  if(wsResyncTimer){
    clearTimeout(wsResyncTimer);
    wsResyncTimer = null;
  }
  // Стоковое поведение: на каждый успешный onOpen целиком пересобираем оболочку страницы.
  continueLoading(playMode); // playMode in variables.js
  // hideSpinner оставляем как было раньше: сам спиннер прячется в .then()/.catch() самих fetch'ей continueLoading.
  // Тут — финальный страховой вызов на случай, если ветка continueLoading отработала синхронно и без сети (повторный reconnect).
  hideSpinner();
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
    // Критичный bootstrap после reconnect: отправляем вне low-очереди,
    // чтобы не ждать WS_SEND_INTERVAL мс * N и быстрее восстановить состояние UI.
    sendWS('getindex=1', true);
    sendWS('gettrackfacts=1', true);
    // getsystem: поля вроде webcpu (полоска CPU) приходят только здесь на странице плеера — без запроса #cpubar остаётся hidden.
    sendWS('getsystem=1', true);
    return;
  }
  if(pathname=='/settings.html'){
    bootstrapSettingsPageWsQueries();
    return;
  }
  if(playMode !== 'player'){
    sendWS('getactive=1');
  }
}

function onClose(event) {
  wserrcnt++;
  clearTimeout(pongtimeout);
  if(wsResyncTimer){
    clearTimeout(wsResyncTimer);
    wsResyncTimer = null;
  }
  // [FIX][SD+WEB] При закрытии сокета останавливаем heartbeat до следующего onOpen.
  stopWsHeartbeat();
  // shellReady НЕ сбрасываем здесь намеренно: DOM прежнего player.html остаётся в браузере.
  // При следующем onOpen стоковая ветка continueLoading() сама перезапишет content корректно.
  // Сброс shellReady в onClose приводил к гонке: retry-таймер запускал второй параллельный
  // continueLoading() до завершения первого, что и давало "чёрный экран до F5".
  // После reboot устройства браузерная вкладка остаётся живой, но состояние mode-bootstrap
  // уже невалидно для нового аптайма ESP. Без явного сброса первое сообщение playermode
  // может пройти как "без изменений", и тяжелая ветка с generatePlaylist() не запустится.
  // Результат: плейлист появляется только после серии ручных F5. Сбрасываем флаг здесь,
  // чтобы после повторного подключения playermode снова отработал как первый старт.
  playerModeBootstrapped = false;
  // Не перетираем #meta при кратковременных reconnect: это воспринимается как "пропал тег".
  // [v0.8.189] Возвращаемся к СТОКОВОЙ логике переподключения: фиксированный интервал 2 секунды
  // в первые 10 попыток. Это критично для сценария "первая подача питания":
  // пока ESP грузится 5-10 сек, экспоненциальный backoff (100ms,2s,4s,8s,15s) попадал
  // в окно готовности ESP только на 4-5-й попытке, давая 15-30 сек пустой страницы → F5.
  // Сток (yoradio-main): каждые 2 сек — попадание в готовность за 2-4 сек, без ручного F5.
  var delay = wserrcnt<10 ? 2000 : 120000;
  
  // Не замораживаем старое состояние UI перед reconnect — ждём актуальные данные от WS.
  // Не пугаем пользователя ложным «потеряна связь» на коротких микропровалах:
  // показываем уведомление только если обрыв длится заметно долго.
  if(wsDisconnectToastTimer){
    clearTimeout(wsDisconnectToastTimer);
    wsDisconnectToastTimer = null;
  }
  if(!wsDisconnectToastShown){
    wsDisconnectToastTimer = setTimeout(function(){
      wsDisconnectToastTimer = null;
      if(websocket && websocket.readyState === WebSocket.OPEN) return;
      notifyWsStatusToast('Потеряна связь с устройством. Пытаемся переподключиться...', true, 2000);
      wsDisconnectToastShown = true;
    }, 3500);
  }
  
  console.log('WebSocket disconnected, reconnecting in', delay, 'ms (attempt', wserrcnt, ')');
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

// --- DLNA WebUI (этап E): HTTP к эндпоинтам этапа D; блок скрыт без dlnaSupported из variables.js. ---
function dlnaPanelVisible(){
  return typeof dlnaSupported !== 'undefined' && dlnaSupported;
}
function dlnaToast(msg, isErr){
  // showToast создаётся при первом setupElement() с WS; до этого — не ломаем клик по DLNA.
  if(typeof window.showToast === 'function') window.showToast(msg, !!isErr);
  else if(typeof console !== 'undefined' && console.log) console.log(isErr ? '[DLNA] ' + msg : msg);
}
function dlnaParseLimit(){
  var el = getId('dlna_limit');
  var n = el ? parseInt(String(el.value).trim(), 10) : 200;
  if(!isFinite(n) || n < 1) n = 200;
  if(n > 65535) n = 65535;
  return n;
}
function dlnaObjectIdParam(){
  var el = getId('dlna_object_id');
  var s = el ? String(el.value).trim() : '0';
  return s.length ? s : '0';
}
function dlnaRefreshContainerList(){
  if(!dlnaPanelVisible()) return;
  var oid = encodeURIComponent(dlnaObjectIdParam());
  fetch(`http://${hostname}/dlna/list?objectId=${oid}&start=0`).then(function(r){ return r.json(); }).then(function(j){
    var ul = getId('dlna_container_list');
    if(!ul) return;
    ul.innerHTML = '';
    if(j.items && j.items.length){
      j.items.forEach(function(it){
        var li = document.createElement('li');
        li.textContent = (it.title != null ? String(it.title) : '') + (it.id != null ? '  ['+it.id+']' : '');
        ul.appendChild(li);
      });
    }else{
      var li0 = document.createElement('li');
      li0.textContent = (j.note || 'Пустой список (полноценный browse — позже).');
      ul.appendChild(li0);
    }
  }).catch(function(){
    dlnaToast('DLNA list: ошибка сети', true);
  });
}
function dlnaAfterSourceSwitch(){
  setPlaylistMod();
  generatePlaylist(`http://${hostname}/api/playlist?`+playlistmod);
  sendWS('getindex=1', true);
}
function initDlnaPlayerUi(){
  var wrap = getId('dlna_panel_wrap');
  if(!wrap) return;
  // Панель должна быть доступна только при поддержке DLNA и только в открытом окне эквалайзера.
  if(!dlnaPanelVisible()){
    wrap.classList.add('hidden');
    return;
  }
  syncDlnaPanelUnderEqualizer();
}
function syncDlnaPanelUnderEqualizer(){
  var wrap = getId('dlna_panel_wrap');
  var eq = getId('equalizerbg');
  if(!wrap || !eq) return;
  // Временный UX-режим: DLNA скрыт на основном экране и показывается только под меню эквалайзера.
  if(dlnaPanelVisible() && !eq.classList.contains('hidden')) wrap.classList.remove('hidden');
  else wrap.classList.add('hidden');
}
function updateDlnaModeStrip(){
  var strip = getId('dlna_mode_strip');
  if(!strip) return;
  if(modedlna){
    strip.textContent = 'Источник: DLNA (UPnP)';
    strip.classList.remove('hidden');
  }else{
    strip.textContent = '';
    strip.classList.add('hidden');
  }
}

function collapseDuplicateMetaText(v){
  var s = (v != null && v !== undefined) ? String(v).trim() : '';
  if(!s || isSystemMetaValue(s)) return s;
  var m = s.match(/^(.+?)\s*-\s*\1$/i);
  if(m && m[1] && m[1].trim().length > 8){
    return m[1].trim();
  }
  return s;
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
    if(typeof data.playermode !== 'undefined') { // Web, SD или DLNA (источник плейлиста)
      // Вычисляем факт реального перехода режима.
      // Повторный приход того же playermode часто случается при bootstrap/reconnect
      // и не должен инициировать тяжёлую перерисовку.
      const pm = data.playermode;
      const nextModeSd = (pm === 'modesd');
      const nextModedlna = (pm === 'modedlna');
      // Смена SD↔WEB и смена WEB↔DLNA должны перезагрузить /api/playlist (разные CSV).
      const modeActuallyChanged = (modesd !== nextModeSd) || (modedlna !== nextModedlna);
      const needHeavyModeSync = (!playerModeBootstrapped) || modeActuallyChanged;

      // [FIX] Сбрасываем sdIndexingActive только при переходе в WEB-режим.
      // В SD-режиме НЕ сбрасываем — playermode может прийти раньше sdindexing=0.
      if(pm !== 'modesd') sdIndexingActive = false;
      // Событие modeSwitching отправляем только при реальном переходе режима
      // (или при первичной инициализации), чтобы не блокировать логику обложек ложными 5с окнами.
      if(needHeavyModeSync){
        document.dispatchEvent(new CustomEvent('modeSwitching', { detail: true }));
        // Сокращаем окно "переключения" до короткого стабилизационного периода.
        setTimeout(() => { document.dispatchEvent(new CustomEvent('modeSwitching', { detail: false })); }, 700);
      }

      modesd = nextModeSd;
      modedlna = nextModedlna;
      updateDlnaModeStrip();
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
      // Тяжёлые действия выполняем только при реальной смене режима (или на первом bootstrap).
      if(needHeavyModeSync){
        setPlaylistMod();
        // Не загружаем плейлист, если SD-индексация ещё идёт —
        // файл может быть не готов (404). sdindexing=0 перезагрузит позже.
        if(!sdIndexingActive) {
          generatePlaylist(`http://${hostname}/api/playlist`+"?"+playlistmod);
        }
        // При смене режима обновляем обложку через логотип, чтобы исключить артефакты старого трека.
        const logo = getId('logo');
        const cover = getId('cover-art-display');
        if(logo) logo.style.display = 'flex';
        if(cover){ cover.style.display = 'none'; cover.innerHTML = ''; }
        // Сбросить глобальное состояние TrackFacts (если доступно).
        if(typeof window.resetTrackFacts === 'function') window.resetTrackFacts();
      }
      // Фиксируем, что первичный playermode уже обработан.
      playerModeBootstrapped = true;
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
      // [FIX] Если сервер явно отклонил ручной запрос факта, диалог больше не нужен:
      // пользователь уже получил причину отказа во всплывающем сообщении.
      // Закрываем диалог сразу, чтобы не оставлять "висящий" экран ожидания.
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
      // Сначала индекс станции, потом поля payload — иначе nameset из того же кадра берёт fromPl по старому currentItem.
      if(typeof data.current !== 'undefined'){
        setCurrentItem(data.current);
      }
      data.payload.forEach(item=> {
        setupElement(item.id, item.value);
      });
      if(typeof data.act !== 'undefined'){
        applySettingsActFromServer(data.act);
      }
      if(typeof data.current !== 'undefined'){
        setCurrentItem(data.current);
      }
    }else{
      if(typeof data.current !== 'undefined') {
        setCurrentItem(data.current);
      }
      if(typeof data.file !== 'undefined') { setPlaylistMod(); generatePlaylist(data.file+"?"+playlistmod); sendWS('submitplaylistdone=1', true); return; }
      if(typeof data.act !== 'undefined'){
        applySettingsActFromServer(data.act);
      }
      if(typeof data.mdns !== 'undefined'){
        const rhost = (hostname==`${data.mdns}.local`)?data.ipaddr:`${data.mdns}.local`;
        getId("radiolink").innerHTML=`<a href="http://${rhost}/settings.html">http://${rhost}/</a>`;
      }
      Object.keys(data).forEach(key=>{
        if(key === 'act') return;
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
  if(value.indexOf('Переподключение') !== -1) return true;
  if(value.indexOf('Error') !== -1) return true;
  if(value.indexOf('Ошибка') !== -1) return true;
  return false;
}
/** true, если строка точно совпадает с именем другой (не текущей) станции в плейлисте — типичный запоздалый nameset после смены частоты. */
function namesetMatchesOtherPlaylistRow(name, curIdx){
  var pl = getId('playlist');
  if(!pl || curIdx == null || curIdx === '') return false;
  var n = String(name == null ? '' : name).trim();
  if(!n) return false;
  var cur = parseInt(curIdx, 10);
  if(isNaN(cur)) return false;
  var hit = false;
  pl.querySelectorAll('li').forEach(function(li){
    var aid = parseInt(li.getAttribute('attr-id'), 10);
    if(isNaN(aid) || aid === cur) return;
    var nm = (li.dataset && li.dataset.name) ? String(li.dataset.name).trim() : '';
    if(nm && nm === n) hit = true;
  });
  return hit;
}
// Видимость #cpubar: прошивка (window.__webCpuBuild) и пользовательский флаг (window.__webCpuUser).
// Пока webcpu из getsystem не пришёл — не трогаем полоску (избегаем гонки wci раньше webcpu).
function applyWebCpuBarVisibility(){
  var bar = getId('cpubar');
  if(!bar) return;
  if(window.__webCpuBuild === undefined) return;
  var buildOn = (window.__webCpuBuild === true || window.__webCpuBuild === 1);
  var userOn = (window.__webCpuUser === undefined || window.__webCpuUser === null)
    ? true
    : (window.__webCpuUser === true || window.__webCpuUser === 1 || window.__webCpuUser === '1');
  if(buildOn && userOn) bar.classList.remove('hidden');
  else bar.classList.add('hidden');
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

  // webcpu — прошивка с WEBUI_CPU_BAR_ENABLE: показываем переключатель #wci на /settings; видимость полоски — ещё и wci.
  if (id === 'webcpu') {
    const en = (value === 1 || value === true || value === '1');
    window.__webCpuBuild = en;
    const cpuCb = getId('wci');
    if (cpuCb) {
      if (en) cpuCb.classList.remove('hidden');
      else cpuCb.classList.add('hidden');
    }
    applyWebCpuBarVisibility();
    return;
  }
  // wci — сохранённое «CPU Info» (полоска внизу плеера).
  if (id === 'wci') {
    window.__webCpuUser = (value === 1 || value === true || value === '1');
    applyWebCpuBarVisibility();
    const el = getId('wci');
    if (el && el.classList.contains('checkbox')) {
      el.classList.remove('checked');
      if (window.__webCpuUser) el.classList.add('checked');
    }
    return;
  }

  if(element){
    if(id=="heap"){
      // Вне экрана плеера не показываем динамические полосы — иначе это выглядит как «симуляция» при update/settings.
      if(!['/','/index.html'].includes(window.location.pathname)) return;
      // Ширина — фактический процент буфера.
      element.style.width=`${value}%`;
      // Цвет — по заданным порогам; сама плавность перехода обеспечивается CSS transition у #heap.
      element.style.backgroundColor = getHeapColorByPercent(value);
      return;
    }
    if(id=="cpubar"){
      // Вне экрана плеера не показываем CPU bar (исключает «ползущую полоску» на /update.html при прошивке/ребуте).
      if(!['/','/index.html'].includes(window.location.pathname)) return;
      // Сервер шлёт cpubar только при WEBUI_CPU_BAR_ENABLE — снимаем hidden, если getsystem ещё не успел (редкий порядок сообщений).
      if (element.classList.contains('hidden')) element.classList.remove('hidden');
      element.style.width=`${value}%`;
      element.style.backgroundColor = getCpuBarColorByPercent(value);
      return;
    }
    if(element.classList.contains("checkbox")){
      element.classList.remove("checked");
      if(value) element.classList.add("checked");
    }
    if(element.classList.contains("classchange")){
      element.attr("class", "classchange");
      element.classList.add(value);
      // [FIX] При STOP немедленно очищаем мета-данные и сбрасываем обложку
      if(id === 'playerwrap' && value === 'stopped') {
        // Сброс обложки на логотип
        var logoEl = getId('logo');
        var coverEl = getId('cover-art-display');
        if(logoEl) logoEl.style.display = 'flex';
        if(coverEl){ coverEl.style.display = 'none'; coverEl.innerHTML = ''; }
        if(typeof closeFactsDialog === 'function') closeFactsDialog();
        if(typeof window.resetTrackFacts === 'function') window.resetTrackFacts();
      }
      // [FIX Задача 2] При начале воспроизведения — если плейлист пуст, перезагружаем его.
      // Покрывает случай: страница загрузилась во время индексации SD, плейлист был пустой,
      // событие sdindexing=0 пропущено или пришло раньше готовности данных.
      if(id === 'playerwrap' && value === 'playing') {
        // Если пользователь нажал PLAY после STOP (без смены станции),
        // показываем station-info коротким окном, как и при switch станции.
        // Это убирает "моргание" и делает поведение единым.
        var nsOnPlay = getId('nameset');
        var metaOnPlay = getId('meta');
        var stationOnPlay = nsOnPlay ? String(nsOnPlay.textContent || '').trim() : '';
        if(metaOnPlay && stationOnPlay){
          var currentMetaOnPlay = String(metaOnPlay.innerText || '').trim();
          if(isSystemMetaValue(currentMetaOnPlay) || !currentMetaOnPlay){
            // --- Вторая строка при старте PLAY без готового TITLE из WS ---
            // Пока нет тега из потока: берём из текущей строки плейлиста жанр (если не «all» и не дубль nameset),
            // иначе коротко показываем hostname потока (если отличается от названия станции);
            // если и этого нет — явный статус [соединение], чтобы строка не оставалась пустой.
            var plForPlay = getId('playlist');
            var rowPlay = plForPlay ? plForPlay.querySelector('li[attr-id="'+currentItem+'"]') : null;
            var gPlay = (rowPlay && rowPlay.dataset && rowPlay.dataset.genre) ? String(rowPlay.dataset.genre).trim() : '';
            var urlPlay = (rowPlay && rowPlay.dataset && rowPlay.dataset.url) ? String(rowPlay.dataset.url).trim() : '';
            var secondLinePlay = '';
            if(gPlay && gPlay.length && gPlay.toLowerCase() !== 'all' &&
               gPlay.toLowerCase() !== stationOnPlay.toLowerCase()){
              secondLinePlay = gPlay;
            } else if(urlPlay){
              try{
                var uPlay = new URL(urlPlay.indexOf('http') === 0 ? urlPlay : 'http://' + urlPlay);
                var hPlay = (uPlay.hostname || '').trim();
                if(hPlay && hPlay.toLowerCase() !== stationOnPlay.toLowerCase()) secondLinePlay = hPlay;
              }catch(ePl){}
            }
            metaOnPlay.innerText = secondLinePlay || '[соединение]';
          }
          window.__metaCoverHoldUntilMs = Date.now() + 4000;
          window.__metaHoldText = '';
          window.__metaHoldPendingMeta = '';
          if(window.__metaCoverDeferredTimer){
            clearTimeout(window.__metaCoverDeferredTimer);
            window.__metaCoverDeferredTimer = null;
          }
          if(window.__metaHoldWsTimer){
            clearTimeout(window.__metaHoldWsTimer);
            window.__metaHoldWsTimer = null;
          }
        }
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
        var metaStrPre = (value != null && value !== undefined) ? String(value) : '';
        // Если после смены станции включено окно удержания station-info,
        // не переключаемся мгновенно на трековый TITLE из WS.
        // Это синхронизирует поведение с вариантом B (4 секунды station-info).
        var holdLeftWsMs = Math.max(0, (window.__metaCoverHoldUntilMs || 0) - now);
        var nsForWs = getId('nameset');
        var nsTxtForWs = nsForWs ? String(nsForWs.textContent || '').trim() : '';
        var metaTxtForWs = metaStrPre.trim();
        var isWsSystem = isSystemMetaValue(metaStrPre);
        var isDuplicateWs = metaTxtForWs && nsTxtForWs &&
          (metaTxtForWs.toLowerCase() === nsTxtForWs.toLowerCase());
        if(!isWsSystem && holdLeftWsMs > 0 && metaTxtForWs && !isDuplicateWs){
          // Строгий hold: 4 секунды держим текущую station-info строку.
          // В этом окне только запоминаем последнее meta для применения ПОСЛЕ hold.
          window.__metaHoldPendingMeta = metaTxtForWs;
          if(!window.__metaHoldWsTimer){
            window.__metaHoldWsTimer = setTimeout(function(){
              var m = getId('meta');
              if(!m){
                window.__metaHoldWsTimer = null;
                window.__metaHoldPendingMeta = '';
                return;
              }
              var pending = String(window.__metaHoldPendingMeta || '').trim();
              if(pending && !isSystemMetaValue(pending)){
                m.innerText = pending;
                lastMetaUpdate = Date.now();
              }
              window.__metaHoldWsTimer = null;
              window.__metaHoldPendingMeta = '';
            }, holdLeftWsMs);
          }
          return;
        }
        var metaIncomingTrim = metaStrPre.trim();
        var metaCurrentTrim = (element.textContent || '').trim();
        // [остановлено], [готов], [соединение] и др. — не троттлим: иначе после частых треков статус Stop не попадёт в #meta.
        if(!isSystemMetaValue(metaStrPre)) {
          // Троттлим только повторы одного и того же текста.
          // Если пришёл новый тег, показываем его сразу (даже если он пришёл вскоре после комментария/статуса).
          if(metaIncomingTrim === metaCurrentTrim && (now - lastMetaUpdate < META_NAMESET_THROTTLE_MS)) return;
          lastMetaUpdate = now;
        }
      }
      if(id=='meta'){
        if(sdIndexingActive && !isSystemMetaValue((value != null && value !== undefined) ? String(value) : '')) return;
        // [FIX] Улучшенная обработка статусов при остановленном плеере
        // Разрешаем обновлять критические статусы даже при остановленном плеере
        if(getId('playerwrap') && getId('playerwrap').classList.contains('stopped')) {
          var currentMetaText = (element.textContent || '').trim();
          var msv = (value != null && value !== undefined) ? String(value) : '';
          if(msv.length > 0) {
            // Разрешаем обновление критических системных статусов
            var isCriticalStatus = 
              msv.indexOf('[соединение]') !== -1 ||
              msv.indexOf('[остановлено]') !== -1 ||
              msv.indexOf('[готов]') !== -1 ||
              msv.indexOf('[Error]') !== -1 ||
              msv.indexOf('[Ошибка]') !== -1;
            
            // Запрещаем только обычные метаданные при остановленном плеере, если это тот же текст
            // (повтор/дребезг). Новый тег (другая строка) пропускаем — иначе при гонке MODE/TITLE
            // после смены станции тег теряется и висит старый до смены композиции в эфире.
            var msvTrim = msv.trim();
            if(!isSystemMetaValue(msv) && !isCriticalStatus && currentMetaText !== '[соединение]' &&
               (msvTrim === currentMetaText || msvTrim.length === 0)) return;
            if(msv.indexOf('[соедин') !== -1 && currentMetaText === '[остановлено]') return;
          }
        }
      }
      let finalValue = value;
      if(id=='meta'){
        finalValue = collapseDuplicateMetaText(finalValue);
        // Если строка meta совпадает с nameset — не дублируем; показываем [соединение] до отличного тега из потока.
        var nsForMeta = getId('nameset');
        var nsTxtForMeta = nsForMeta ? String(nsForMeta.textContent || '').trim() : '';
        var metaTxtForCompare = (finalValue != null && finalValue !== undefined) ? String(finalValue).trim() : '';
        var holdLeftForDedupe = Math.max(0, (window.__metaCoverHoldUntilMs || 0) - Date.now());
        if(holdLeftForDedupe <= 0 &&
           metaTxtForCompare &&
           nsTxtForMeta &&
           !isSystemMetaValue(metaTxtForCompare) &&
           metaTxtForCompare.toLowerCase() === nsTxtForMeta.toLowerCase()){
          // Дубль первой строки — не оставляем пустоту: до прихода реального тега показываем статус соединения.
          finalValue = '[соединение]';
        }
      }
      if(id=='nameset'){
        var rawNs = (value != null && value !== undefined) ? String(value).trim() : '';
        var plN = getId('playlist');
        var rowN = plN ? plN.querySelector('li[attr-id="'+currentItem+'"]') : null;
        var fromPl = (rowN && rowN.dataset && rowN.dataset.name) ? String(rowN.dataset.name).trim() : '';
        if(rawNs.length > 0) {
          // Запоздалый STATIONNAME с именем прошлой станции (тот же текст, что у другой строки плейлиста) — не затирать текущую.
          if(fromPl && rawNs !== fromPl && namesetMatchesOtherPlaylistRow(rawNs, currentItem)) {
            finalValue = fromPl;
            lastNamesetText = fromPl;
          } else {
            lastNamesetText = rawNs;
            finalValue = rawNs;
          }
        } else {
          // Пустой nameset от ESP при смене станции — не подставлять прошлое имя; взять строку текущей позиции из плейлиста.
          finalValue = fromPl;
          lastNamesetText = fromPl;
        }
        var nowNs = Date.now();
        var shownNs = (element.textContent || '').trim();
        if(String(finalValue).trim() === shownNs && shownNs !== '') {
          if(nowNs - lastNamesetUpdate < META_NAMESET_THROTTLE_MS) return;
        }
        lastNamesetUpdate = nowNs;
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
  const playlist = getId("playlist");
  // На /settings.html и др. страницах без плейлиста не трогаем индекс — иначе exception рвёт onMessage
  // и не отрабатывает payload того же кадра (чекбоксы остаются в HTML-дефолте до F5).
  if(!playlist) return;
  var idx = typeof item === 'number' ? item : parseInt(item, 10);
  if(isNaN(idx)) idx = currentItem;
  var changed = idx !== currentItem;
  if(changed){
    lastNamesetText = '';
    lastNamesetUpdate = 0;
    // При смене станции сбрасываем строку трега: иначе до прихода нового TITLE
    // остаётся текст прошлой станции и создаётся впечатление «скрипт не подгрузился».
    var metaOnSwitch = getId('meta');
    if(metaOnSwitch){
      // Если плеер остановлен — только фиксируем статус в #meta; НИКОГДА не делаем return из setCurrentItem,
      // иначе не обновятся currentItem и подсветка плейлиста (плейлист «пропадает» / ломается логика WS).
      var wrap = getId('playerwrap');
      if(wrap && wrap.classList.contains('stopped')){
        metaOnSwitch.innerText = '[остановлено]';
        lastMetaUpdate = 0;
      } else {
      // Не оставляем #meta пустым при переключении станции в режиме воспроизведения.
      // Приоритет UX (как раньше): кратко показать инфо станции, затем title из /api/current-cover;
      // если title совпадает с именем станции — не дублировать второй строкой (см. ниже dedupe в setupElement).
      var stationMetaFallback = '';
      var rowMeta = playlist.querySelector('li[attr-id="'+idx+'"]');
      if(rowMeta && rowMeta.dataset && rowMeta.dataset.name){
        stationMetaFallback = String(rowMeta.dataset.name).trim();
      }
      metaOnSwitch.innerText = stationMetaFallback || '[соединение]';
      lastMetaUpdate = 0;
      // Окно удержания: сглаживаем гонку WS-кадров при смене станции.
      window.__metaCoverHoldUntilMs = Date.now() + 4000;
      window.__metaHoldText = '';
      window.__metaHoldPendingMeta = '';
      if(window.__metaCoverDeferredTimer){
        clearTimeout(window.__metaCoverDeferredTimer);
        window.__metaCoverDeferredTimer = null;
      }

      // Вариант B: синхронизируем #meta с тем, что соответствует текущей обложке.
      if(typeof window.__metaCoverAbortController !== 'undefined' && window.__metaCoverAbortController){
        try{ window.__metaCoverAbortController.abort(); }catch(e){}
      }
      var canAbort = (typeof AbortController !== 'undefined');
      var ac = canAbort ? new AbortController() : null;
      window.__metaCoverAbortController = ac;
      if(typeof window.__metaCoverSyncSeq !== 'undefined') {
        window.__metaCoverSyncSeq++;
      } else {
        window.__metaCoverSyncSeq = 1;
      }
      var mySeq = window.__metaCoverSyncSeq;

      var coverFetchTimeout = setTimeout(function(){
        if(window.__metaCoverAbortController){
          try{ window.__metaCoverAbortController.abort(); }catch(e){}
        }
      }, 2500);

      fetch('/api/current-cover?t=' + Date.now(), ac ? { signal: ac.signal } : undefined)
        .then(function(r){ if(!r || !r.ok) throw new Error('cover fetch'); return r.json(); })
        .then(function(data){
          if(mySeq !== window.__metaCoverSyncSeq) return;
          if(!metaOnSwitch) return;
          clearTimeout(coverFetchTimeout);
          var titleFromCover = (data && data.title) ? String(data.title).trim() : '';
          if(titleFromCover && !isSystemMetaValue(titleFromCover)){
            var titleLooksSameAsStation =
              stationMetaFallback &&
              titleFromCover.toLowerCase() === stationMetaFallback.toLowerCase();
            var holdLeftMs = Math.max(0, (window.__metaCoverHoldUntilMs || 0) - Date.now());
            if(!titleLooksSameAsStation && holdLeftMs > 0){
              window.__metaCoverDeferredTimer = setTimeout(function(){
                if(mySeq !== window.__metaCoverSyncSeq) return;
                if(!metaOnSwitch) return;
                metaOnSwitch.innerText = titleFromCover;
                lastMetaUpdate = Date.now();
                window.__metaCoverDeferredTimer = null;
              }, holdLeftMs);
            } else {
              metaOnSwitch.innerText = titleFromCover;
              lastMetaUpdate = Date.now();
            }
            return;
          }
          var ns = getId('nameset');
          if(ns && ns.textContent){
            var nsTxt = String(ns.textContent).trim();
            // Не копируем nameset во вторую строку, если это то же имя, что у активной строки плейлиста.
            var dupPl = stationMetaFallback && nsTxt.toLowerCase() === stationMetaFallback.toLowerCase();
            metaOnSwitch.innerText = (nsTxt && !dupPl) ? nsTxt : '[соединение]';
          } else if (stationMetaFallback) {
            metaOnSwitch.innerText = stationMetaFallback;
          } else {
            metaOnSwitch.innerText = '[соединение]';
          }
        })
        .catch(function(){
          // Ошибка cover API — оставляем уже показанный fallback (stationMetaFallback / [соединение]).
        });
      }
    }
  }
  currentItem = idx;
  let topPos = 0, lih = 0;
  playlist.querySelectorAll('li').forEach((item, index)=>{
      // Не перезаписываем class целиком: иначе теряются служебные классы фильтров
      // (например, .search-hidden), и поиск "ломается" после первого запуска/PLAY.
      item.classList.add("play");
      item.classList.remove("active");
      if(index+1===currentItem){
          item.classList.add("active");
          topPos = item.offsetTop;
          lih = item.offsetHeight;
      }
  });
  // Центрируем активную строку в плейлисте.
  // В некоторых Android-браузерах `scrollTo({top,...})` иногда игнорируется — используем `scrollTop`.
  var desiredTop = (topPos - playlist.offsetHeight/2 + lih/2);
  if(desiredTop < 0) desiredTop = 0;
  var maxTop = playlist.scrollHeight - playlist.clientHeight;
  if(maxTop < 0) maxTop = 0;
  if(desiredTop > maxTop) desiredTop = maxTop;
  if (typeof requestAnimationFrame === 'function') {
    requestAnimationFrame(function(){
      playlist.scrollTop = desiredTop;
    });
  } else {
    playlist.scrollTop = desiredTop;
  }
  // Имя станции из плейлиста — опора при каждом {"current":N}: иначе запоздалый nameset оставлял прошлую станцию при том же индексе.
  var nsSync = getId('nameset');
  if(playlist && nsSync){
    var rowS = playlist.querySelector('li[attr-id="'+idx+'"]');
    if(rowS && rowS.dataset && rowS.dataset.name){
      var nmS = String(rowS.dataset.name).trim();
      if(nmS){
        nsSync.textContent = nmS;
        lastNamesetText = nmS;
        lastNamesetUpdate = Date.now();
      }
    }else if(changed){
      nsSync.textContent = '';
    }
  }
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
    
    html+=`<li class="pleitem" id="${'plitem'+index}"><span class="grabbable" draggable="true">${String(index+1).padStart(3, '0')}</span>
      <span class="pleinput plecheck"><input type="checkbox" class="plcb" /></span>
      <input class="pleinput plename" type="text" value="${quoteattr(pName)}" maxlength="140" />
      <input class="pleinput pleurl" type="text" value="${pUrl}" maxlength="140" />
      <span class="pleinput pleplay" data-command="preview">&#9658;</span>
      <input class="pleinput pleovol" type="number" min="-64" max="64" step="1" value="${pOvol}" />
      <input class="pleinput plegenre" type="text" value="${quoteattr(pGenre)}" maxlength="50" onchange="autoSaveGenre(this)" onblur="autoSaveGenre(this)" />
      <input class="pleinput plefavorite" type="number" min="0" max="1" step="1" value="${pFavorite}" />
      </li>`;
  });
  ple.innerHTML=html;
}

/* AUTO-SAVE FUNCTION for Genres (debounce — onchange + быстрый ввод) */
var genreSaveTimer = null;
function autoSaveGenre(input) {
  let liIndex = input.closest('li').id.replace('plitem', '');
  let playlistItem = getId('playlist').children[liIndex];
  if (playlistItem) {
    playlistItem.dataset.genre = input.value;
    playlistItem.setAttribute('data-genre', input.value);
  }
  clearTimeout(genreSaveTimer);
  genreSaveTimer = setTimeout(function(){ submitPlaylist(true); }, 500);
}

function handlePlaylistData(fileData) {
  const ul = getId('playlist');
  if(!ul) return;
  ul.innerHTML='';
  if (!fileData || !String(fileData).trim()) return;
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
  if(!html.length){
    var metaPl = getId('meta');
    if(metaPl) metaPl.innerText = 'Плейлист пуст или файл не содержит станций';
  }
  setCurrentItem(currentItem);
  // Если поиск уже открыт/введён, применяем фильтр повторно после перезагрузки плейлиста.
  // Это сохраняет результаты поиска и позволяет делать повторные поиски без F5.
  filterPlaylistBySearch();
  updateGenreList();
  // Всегда пересобираем строки редактора из актуального #playlist (и WEB, и SD).
  // Раньше в SD initPLEditor() не вызывался — pleditorcontent оставался старым, и следующий
  // submitPlaylist (избранное и т.д.) перезаписывал playlist.csv без жанров.
  if (getId('pleditorcontent')) initPLEditor();
  // Успешная отрисовка плейлиста: сбрасываем отложенные ретраи и счётчик.
  if(playlistRetryTimer){
    clearTimeout(playlistRetryTimer);
    playlistRetryTimer = null;
  }
  playlistRetryCount = 0;
  if(playlistDomWaitTimer){
    clearTimeout(playlistDomWaitTimer);
    playlistDomWaitTimer = null;
  }
  playlistDomWaitCount = 0;
}

function schedulePlaylistRetry(path){
  // Не уходим в бесконечный цикл: максимум PLAYLIST_RETRY_MAX попыток.
  if(playlistRetryCount >= PLAYLIST_RETRY_MAX) return;
  playlistRetryCount++;
  if(playlistRetryTimer){
    clearTimeout(playlistRetryTimer);
    playlistRetryTimer = null;
  }
  // Через короткую паузу пробуем ещё раз тот же URL плейлиста.
  playlistRetryTimer = setTimeout(function(){
    generatePlaylist(path);
  }, PLAYLIST_RETRY_DELAY_MS);
}

function generatePlaylist(path){
  // [FIX v3] Защита от двойного вызова: если плейлист уже грузится — пропускаем.
  if(bigplaylist) return;
  path = path.replace(/:\/\/.+?\//, `://${hostname}/`);
  var plEl = getId('playlist');
  // Нет контейнера — шелл ещё не подгрузился; короткие повторы, без «тихого» return и без лимита ретраев пустого API.
  if(!plEl){
    if(playlistDomWaitCount >= PLAYLIST_DOM_WAIT_MAX){
      playlistDomWaitCount = 0;
      return;
    }
    playlistDomWaitCount++;
    if(playlistDomWaitTimer){ clearTimeout(playlistDomWaitTimer); playlistDomWaitTimer = null; }
    playlistDomWaitTimer = setTimeout(function(){
      playlistDomWaitTimer = null;
      generatePlaylist(path);
    }, PLAYLIST_DOM_WAIT_MS);
    return;
  }
  playlistDomWaitCount = 0;
  var savedHtml = plEl.innerHTML;
  plEl.innerHTML='<div class="pl-load-overlay"><span class="pl-load-spin"></span></div>';
  bigplaylist = true;
  fetch(path).then(response => response.text()).then(plcontent => {
    var txt = (plcontent != null) ? String(plcontent) : '';
    if(!txt.trim()){
      // Пустой ответ на старте: оставляем прежний DOM и планируем авто-повтор,
      // чтобы плейлист появился сам после прогрузки устройства, без F5.
      plEl.innerHTML = savedHtml;
      schedulePlaylistRetry(path);
      return;
    }
    handlePlaylistData(txt);
  }).catch(() => {
    // Сетевой сбой/таймаут на старте: восстанавливаем DOM и делаем ограниченный авто-повтор.
    plEl.innerHTML = savedHtml;
    schedulePlaylistRetry(path);
  }).finally(function(){
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
  plitem.innerHTML = '<span class="grabbable" draggable="true">'+String(cnt.length+1).padStart(3, '0')+'</span>\
      <span class="pleinput plecheck"><input type="checkbox" /></span>\
      <input class="pleinput plename" type="text" value="" maxlength="140" />\
      <input class="pleinput pleurl" type="text" value="" maxlength="140" />\
      <span class="pleinput pleplay" data-command="preview">&#9658;</span>\
      <input class="pleinput pleovol" type="number" min="-30" max="30" step="1" value="0" />\
      <input class="pleinput plegenre" type="text" value="" maxlength="50" onchange="autoSaveGenre(this)" onblur="autoSaveGenre(this)" />\
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
    items[i].getElementsByTagName('span')[0].innerText=String(i+1).padStart(3, '0');
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
    var row = items[i];
    var elName = row.querySelector('.plename');
    var elUrl = row.querySelector('.pleurl');
    var elOvol = row.querySelector('.pleovol');
    var elGenre = row.querySelector('.plegenre');
    var elFav = row.querySelector('.plefavorite');
    if(!elName || !elUrl || !elOvol) continue;
    if(elName.value == "" || elUrl.value == "") continue;
    let ovol = elOvol.value;
    if(ovol < -30) ovol = -30;
    if(ovol > 30) ovol = 30;
    let genre = elGenre ? elGenre.value : '';
    let favorite = elFav ? elFav.value : '0';
    output+=elName.value+"\t"+elUrl.value+"\t"+ovol+"\t"+genre+"\t"+favorite+"\n";
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
    // После открытия/закрытия эквалайзера синхронизируем видимость DLNA-блока.
    if(id=='equalizerbg') syncDlnaPanelUnderEqualizer();
  }
}
function checkboxClick(cb, command){
  cb.classList.toggle("checked");
  sendWS(`${command}=${cb.classList.contains("checked")?1:0}`, true);
}
var _sliderWsDebounce = {};
function sliderInput(sl, command){
  fillSlider(sl);
  clearTimeout(_sliderWsDebounce[command]);
  _sliderWsDebounce[command] = setTimeout(function(){
    delete _sliderWsDebounce[command];
    sendWS(`${command}=${sl.value}`, true);
  }, 50);
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
  // Заменяем содержимое страницы коротким сообщением и блокируем меню,
  // чтобы пользователь не пытался кликнуть что-то в момент перезагрузки ESP.
  getId("settingscontent").innerHTML=`<h2>${info}</h2>`;
  getId("settingsdone").classList.add("hidden");
  getId("navigation").classList.add("hidden");
  // Ждём готовности веб-сервера и автоматически переходим на корень — без ручного F5.
  waitForWebReadyAndRedirect('/', 4000);
}
// Надёжное ожидание готовности веб-сервера после reboot.
// Стоковая прошивка делает простой setTimeout 5 сек + redirect, потому что грузится быстро.
// Модификация (DLNA, PSRAM-кэш, LittleFS, индексация SD, AsyncTCP) поднимается дольше,
// поэтому короткого таймаута не хватает и пользователь видит "сайт недоступен" → F5.
// Решение: после короткой паузы цикл XHR-проб к /variables.js (это самый лёгкий эндпоинт,
// отдаётся sprintf-буфером, не читает LittleFS). Первый успешный ответ — мгновенный redirect.
function waitForWebReadyAndRedirect(targetPath, startDelayMs){
  var path = targetPath || '/';
  // Стартовая пауза: пока ESP физически перезагружается, любые пробы заведомо неуспешны.
  var startDelay = (typeof startDelayMs === 'number' && startDelayMs >= 0) ? startDelayMs : 0;
  // Жёсткий дедлайн на цикл проб: 90 секунд (с большим запасом на медленный Wi‑Fi/инициализацию FS).
  var deadlineMs = Date.now() + 90000;
  // Эндпоинт-индикатор готовности (отдаёт несколько байт текста без обращения к LittleFS).
  var probeUrl = `http://${hostname}/variables.js?_=` + Date.now();
  function doRedirect(){
    // Используем replace, чтобы reboot-сообщение не оставалось в истории браузера ("Назад").
    window.location.replace(`http://${hostname}${path}`);
  }
  function probe(){
    // Если прошлый exhange длинный — XHR с явным timeout надёжнее fetch на мобильных браузерах.
    var xhr;
    try{ xhr = new XMLHttpRequest(); }catch(e){ setTimeout(probe, 1200); return; }
    xhr.timeout = 2500;
    xhr.onload = function(){
      // Любой 2xx подтверждает: webserver поднят и обрабатывает запросы — можно переходить.
      if(xhr.status >= 200 && xhr.status < 300){ doRedirect(); return; }
      if(Date.now() < deadlineMs){ setTimeout(probe, 1200); }
      else doRedirect();
    };
    xhr.onerror = function(){
      if(Date.now() < deadlineMs){ setTimeout(probe, 1200); }
      else doRedirect();
    };
    xhr.ontimeout = xhr.onerror;
    try{ xhr.open('GET', probeUrl, true); xhr.send(null); }
    catch(e){
      if(Date.now() < deadlineMs){ setTimeout(probe, 1200); }
      else doRedirect();
    }
  }
  setTimeout(probe, startDelay);
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
    // После сохранения Wi‑Fi устройство перезагружается дольше обычного;
    // используем тот же надёжный механизм ожидания готовности веба.
    waitForWebReadyAndRedirect('/', 10000);
  }
}
function playItem(target){
  const item = target.attr('attr-id');
  setCurrentItem(item)
  sendWS(`play=${item}`, true);
}
function hideSpinner(){
  // Без null-check ранний вызов (до полной оболочки из netserver) ломал весь JS → «пустой экран до F5».
  var pr = getId("progress");
  if(pr) pr.classList.add("hidden");
  var ct = getId("content");
  if(ct) ct.classList.remove("hidden");
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

/** Дисклеймер ИИ: по factsAi с прошивки; при отсутствии поля — эвристика tfprovider + префиксы iTunes:/Last.fm. */
function trackFactsAiDisclaimerFromData(data) {
  const tf = data && data.tfprovider != null ? data.tfprovider : null;
  let fa = null;
  if (data && data.factsAi !== undefined && data.factsAi !== null) {
    fa = Number(data.factsAi) === 1;
  }
  const factsArr = (data && data.facts) || [];
  if (typeof window.trackFactsAiDisclaimerHtml === 'function') {
    return window.trackFactsAiDisclaimerHtml(tf, fa, factsArr);
  }
  for (let i = 0; i < factsArr.length; i++) {
    const s = String(factsArr[i] || '').trim();
    if (s.indexOf('iTunes:') === 0 || s.indexOf('Last.fm') === 0) return '';
  }
  const p = tf != null ? Number(tf) : 2;
  if (fa === false) return '';
  if (fa === true || p === 0 || p === 1 || p === 4) {
    return '<p class="no-facts" style="opacity:0.72;font-size:0.85em;margin-top:0.5em">Текст сгенерирован ИИ; возможны неточности.</p>';
  }
  return '';
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

// Повторный тап по обложке/лого при открытом диалоге: следующий фрагмент AI-серии или закрытие окна.
async function factsDialogSecondTap() {
  if (!trackFactsEnabled) {
    closeFactsDialog();
    return;
  }
  if (isPlayerStoppedDom()) {
    closeFactsDialog();
    return;
  }
  try {
    const response = await fetch('/api/current-fact?t=' + Date.now());
    if (!response.ok) {
      closeFactsDialog();
      return;
    }
    const data = await response.json();
    if (!trackFactsManualRequestAllowed(data.title)) {
      closeFactsDialog();
      return;
    }
    const incomplete =
      Number(data.incompleteIterative) === 1 || data.incompleteIterative === true;
    if (!incomplete) {
      closeFactsDialog();
      return;
    }
    sendWS('trackfactsrequest=1');
    const content = getId('factsdialog-content');
    if (content) {
      const facts = (data.facts || []).filter(f => f && f.trim().length > 0);
      const factsHtml = facts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('');
      const aiNote = trackFactsAiDisclaimerFromData(data);
      content.innerHTML =
        factsHtml + aiNote + '<p class="no-facts" style="opacity:0.88">⏳ Загружаем остальные фрагменты…</p>';
    }
    if (factsDialogTimer) {
      clearTimeout(factsDialogTimer);
      factsDialogTimer = null;
    }
    factsDialogTimer = setTimeout(() => {
      closeFactsDialog();
    }, 30000);
    pollFactsDialogUntilReady(90000);
  } catch (e) {
    closeFactsDialog();
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
    factsDialogSecondTap();
    return;
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
  // Радио одно ядро занято звуком; ответ AI/сеть могут занять до ~1–2 мин при серии фактов.
  const POLL_MS = 1500;
  const HARD_CAP_MS = Math.max(maxMs || 0, 90000) + 60000;
  const startedAt = Date.now();
  const content = getId('factsdialog-content');
  const header = getId('factsdialog-header');
  if (!content) return;

  const tick = async () => {
    if (!window.factsDialogOpen) {
      factsDialogPollTimer = null;
      return;
    }
    if (isPlayerStoppedDom()) {
      closeFactsDialog();
      return;
    }

    try {
      const response = await fetch('/api/current-fact?t=' + Date.now());
      if (!response.ok) {
        content.innerHTML = '<p class="no-facts">Не удалось связаться с устройством</p>';
        factsDialogPollTimer = null;
        return;
      }
      const data = await response.json();
      if (isPlayerStoppedDom()) {
        closeFactsDialog();
        return;
      }
      const facts = (data.facts || []).filter(f => f && f.trim().length > 0);
      const stillPending = Number(data.pending) === 1 || data.pending === true;
      const manualMore = Number(data.manualMore) === 1 || data.manualMore === true;
      const elapsed = Date.now() - startedAt;
      if (header && data.title) {
        header.innerHTML = '💡 ' + escapeHtmlFacts(data.title);
      }

      if (facts.length > 0) {
        setTrackFactsCache(data.title || '', facts);
        const factsHtml = facts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('');
        const aiNote = trackFactsAiDisclaimerFromData(data);
        if (!stillPending) {
          const incompletePoll =
            !isPlayerStoppedDom() &&
            (Number(data.incompleteIterative) === 1 || data.incompleteIterative === true);
          const hintPoll = incompletePoll
            ? '<p class="no-facts" style="opacity:0.85;font-size:0.9em">Нажмите на обложку или лого ещё раз — следующий фрагмент.</p>'
            : '';
          content.innerHTML = factsHtml + aiNote + hintPoll;
          factsDialogPollTimer = null;
          return;
        }
        if (elapsed < HARD_CAP_MS) {
          const incomplete =
            Number(data.incompleteIterative) === 1 || data.incompleteIterative === true;
          const showLoadingMore =
            stillPending && (manualMore || incomplete);
          const tail = showLoadingMore
            ? '<p class="no-facts" style="opacity:0.88">⏳ Загружаем остальные фрагменты…</p>'
            : '';
          content.innerHTML = factsHtml + aiNote + tail;
          factsDialogPollTimer = setTimeout(tick, POLL_MS);
          return;
        }
        content.innerHTML = factsHtml + aiNote;
        factsDialogPollTimer = null;
        return;
      }

      const cached = window.trackFactsCache || { title: '', facts: [] };
      const cachedFacts = (cached.facts || []).filter(f => f && f.trim().length > 0);
      if (!stillPending && cachedFacts.length > 0 && data.title && cached.title === data.title) {
        const aiNoteC = trackFactsAiDisclaimerFromData(data);
        content.innerHTML = cachedFacts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('') + aiNoteC;
        factsDialogPollTimer = null;
        return;
      }

      if (stillPending) {
        if (elapsed < HARD_CAP_MS) {
          const leftSec = Math.max(1, Math.ceil((HARD_CAP_MS - elapsed) / 1000));
          content.innerHTML =
            '<p class="no-facts">⏳ Готовим текст о треке… Обычно до 30–60 с. Окно можно закрыть — результат появится под обложкой.</p>' +
            '<p class="no-facts" style="opacity:0.75;font-size:0.9em">Осталось до тайм-аута: ~' + leftSec + ' с</p>';
          factsDialogPollTimer = setTimeout(tick, POLL_MS);
          return;
        }
        content.innerHTML =
          '<p class="no-facts">За отведённое время ответ не успел прийти. Проверьте интернет и нажмите на обложку ещё раз.</p>';
        factsDialogPollTimer = null;
        return;
      }

      if (!trackFactsManualRequestAllowed(data.title)) {
        closeFactsDialogAndNotifyTrackFactsBlocked(data.title);
        return;
      }
      if (data.status && data.status.length > 0) {
        content.innerHTML = '<p class="no-facts">' + escapeHtmlFacts(data.status) + '</p>';
      } else {
        content.innerHTML =
          '<p class="no-facts">Текста о треке пока нет. Нажмите на обложку ещё раз через несколько секунд или смените композицию.</p>';
      }
      factsDialogPollTimer = null;
    } catch (e) {
      content.innerHTML = '<p class="no-facts">Ошибка загрузки. Повторите попытку.</p>';
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
      if (cachedFacts.length > 0 && cached.title && data.title && cached.title === data.title &&
          trackFactsManualRequestAllowed(data.title)) {
        const aiNoteC = trackFactsAiDisclaimerFromData(data);
        content.innerHTML = cachedFacts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('') + aiNoteC;
        return;
      }
      if (!trackFactsManualRequestAllowed(data.title)) {
        closeFactsDialogAndNotifyTrackFactsBlocked(data.title);
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
      pollFactsDialogUntilReady(90000);
      return;
    }
    
    // Если факты пришли от API — сразу обновляем глобальный кеш.
    setTrackFactsCache(data.title || '', facts);
    const incompleteHint =
      !isPlayerStoppedDom() &&
      (Number(data.incompleteIterative) === 1 || data.incompleteIterative === true);
    const hintHtml = incompleteHint
      ? '<p class="no-facts" style="opacity:0.85;font-size:0.9em">Нажмите на обложку или лого ещё раз — следующий фрагмент.</p>'
      : '';
    const aiNoteOpen = trackFactsAiDisclaimerFromData(data);
    content.innerHTML =
      facts.map(f => '<p>💡 ' + escapeHtmlFacts(f) + '</p>').join('') + aiNoteOpen + hintHtml;
    const stillPendingOpen = Number(data.pending) === 1 || data.pending === true;
    if (stillPendingOpen) {
      pollFactsDialogUntilReady(90000);
    }
  } catch (e) {
    content.innerHTML = '<p class="no-facts">Ошибка загрузки фактов</p>';
  }
}

function escapeHtmlFacts(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

function isPlayerStoppedDom() {
  const w = getId('playerwrap');
  return !!(w && w.classList.contains('stopped'));
}

/**
 * Строка под названием станции (#meta) обновляется по WS быстрее, чем «остывает» metaTitle в /api/current-fact.
 * Без этой проверки API может ещё отдавать старый ICY-трек, а в UI уже [остановлено] — и показывался лишний диалог.
 */
function trackFactsUiLineIsStoppedOrSystem() {
  const m = getId('meta');
  if (!m) return false;
  const raw = String(m.textContent || m.innerText || '').trim();
  if (!raw) return false;
  if (/\[(остановлено|stopped)\]/i.test(raw)) return true;
  const first = raw.split(/\n/)[0].trim();
  return isSystemMetaValue(first);
}

let _tfManualBlockedToastAt = 0;
function notifyTrackFactsManualBlocked(apiTitle) {
  const now = Date.now();
  if (now - _tfManualBlockedToastAt < 2200) return;
  _tfManualBlockedToastAt = now;
  if (typeof window.showToast !== 'function') return;
  const t = (apiTitle != null && apiTitle !== undefined) ? String(apiTitle).trim() : '';
  const playbackBlocked =
    isPlayerStoppedDom() ||
    trackFactsUiLineIsStoppedOrSystem() ||
    /\[(остановлено|stopped)\]/i.test(t);
  window.showToast(
    playbackBlocked
      ? 'Запрос факта доступен только во время воспроизведения.'
      : 'Сейчас в строке трека нет названия композиции для запроса факта.',
    true
  );
}

function closeFactsDialogAndNotifyTrackFactsBlocked(apiTitle) {
  notifyTrackFactsManualBlocked(apiTitle);
  closeFactsDialog();
}

/** Ручной сетевой запрос факта по обложке — только при воспроизведении и когда title не служебный статус. */
function trackFactsManualRequestAllowed(apiTitle) {
  if (isPlayerStoppedDom()) return false;
  if (trackFactsUiLineIsStoppedOrSystem()) return false;
  const t = (apiTitle != null && apiTitle !== undefined) ? String(apiTitle).trim() : '';
  if (t.length > 0 && isSystemMetaValue(t)) return false;
  return true;
}


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
        initDlnaPlayerUi();
        audiopreview=getId('audiopreview');
        initClock();
        setTimeout(() => {
          // Один раз на сессию: иначе при каждом повторном fetch player.html навешивались новые слушатели
          // и поведение (в т.ч. закрытие окон) становилось непредсказуемым.
          if(!window.__lstepPlayerOutsideUiBound){
            window.__lstepPlayerOutsideUiBound = true;
            document.addEventListener('click', function(e) {
            const dialog = getId('genredialog');
            const genreBtn = getId('genrebutton');
            
            if (dialog && dialog.style.display === 'block' && 
                !dialog.contains(e.target) && 
                !genreBtn.contains(e.target)) {
              dialog.style.display = 'none';
              genreBtn.classList.remove('active');
            }
            const equalizer = getId('equalizerbg');
            const eqBtn = getId('eqalbutton');
            if (equalizer && !equalizer.classList.contains('hidden') &&
                !equalizer.contains(e.target) &&
                (!eqBtn || !eqBtn.contains(e.target))) {
              // Клик вне окна эквалайзера закрывает его и вложенную DLNA-панель.
              equalizer.classList.add('hidden');
              if (eqBtn) eqBtn.classList.remove('active');
              syncDlnaPanelUnderEqualizer();
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
          }
          
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
        // Критичные запросы состояния отправляем immediate,
        // чтобы интерфейс быстрее выходил в консистентное состояние после загрузки страницы.
        sendWS('getindex=1', true);
        sendWS('gettrackfacts=1', true); // [FIX] Запрашиваем состояние TrackFacts для player page
        // Синхронизация webcpu и прочих системных флагов (полоска CPU внизу включается по полю webcpu).
        sendWS('getsystem=1', true);
      }).catch(function(){
        // Стоковая ветка: при ошибке сети просто снимаем спиннер. Повторная сборка shell
        // произойдёт автоматически при следующем onOpen WS (например, после reboot).
        hideSpinner();
      });
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
        bootstrapSettingsPageWsQueries();
        // Навешиваем обработчик смены провайдера для обновления описания ключа
        setTimeout(() => {
          let sel = getId('tfprovider');
          if (sel) {
            sel.onchange = updateKeyDesc;
            updateKeyDesc(false); // [FIX] НЕ отправляем на сервер при инициализации — значение ещё не загружено с ESP32!
          }
        }, 100);
        getWiFi(`http://${hostname}/data/wifi.csv`+"?"+new Date().getTime());
        classEach("reset", function(el){ el.innerHTML='<svg viewBox="0 0 16 16" class="fill"><path d="M8 3v5a36.973 36.973 0 0 1-2.324-1.166A44.09 44.09 0 0 1 3.417 5.5a52.149 52.149 0 0 1 2.26-1.32A43.18 43.18 0 0 1 8 3z"/><path d="M7 5v1h4.5C12.894 6 14 7.106 14 8.5S12.894 11 11.5 11H1v1h10.5c1.93 0 3.5-1.57 3.5-3.5S13.43 5 11.5 5h-4z"/></svg>'; });
        // Загружаем информацию о файловой системе в TOOLS
        loadFsInfo();
      }).catch(function(){
        // Сток: при ошибке сети просто снимаем спиннер. Повтор инициирует следующий onOpen WS.
        hideSpinner();
      });
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
      }).catch(function(){
        // Сток: при ошибке сети просто снимаем спиннер. Повтор инициирует следующий onOpen WS.
        hideSpinner();
      });
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
      }).catch(function(){
        // Сток: при ошибке сети просто снимаем спиннер. Повтор инициирует следующий onOpen WS.
        hideSpinner();
      });
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
      if(window.location.pathname=='/settings.html'){
        bootstrapSettingsPageWsQueries();
      }else{
        sendWS('getactive=1', true);
      }
    }).catch(function(){
      // Сток (AP-ветка): при ошибке сети просто снимаем спиннер.
      // Повтор сборки shell инициирует следующий onOpen WS.
      hideSpinner();
    });
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

        case "dlna_init":
          fetch(`http://${hostname}/dlna/init`).then(function(r){ return r.json(); }).then(function(j){
            dlnaToast((j.ok || j.queued) ? 'DLNA: init в очереди worker' : 'DLNA: очередь worker занята', !(j.ok || j.queued));
          }).catch(function(){ dlnaToast('DLNA init: ошибка сети', true); });
          break;
        case "dlna_status":
          fetch(`http://${hostname}/dlna/status`).then(function(r){ return r.json(); }).then(function(j){
            var err = (j.error != null && String(j.error).length) ? (' | ' + j.error) : '';
            dlnaToast('DLNA: busy=' + j.busy + ' jobs=' + j.processed + err, false);
          }).catch(function(){ dlnaToast('DLNA status: ошибка сети', true); });
          break;
        case "dlna_refresh":
          dlnaRefreshContainerList();
          break;
        case "dlna_build":
          fetch(`http://${hostname}/dlna/build?objectId=${encodeURIComponent(dlnaObjectIdParam())}&limit=${dlnaParseLimit()}`).then(function(r){ return r.json(); }).then(function(j){
            dlnaToast(j.queued ? 'DLNA: build в очереди' : 'DLNA: build не поставлен', !j.queued);
          }).catch(function(){ dlnaToast('DLNA build: ошибка сети', true); });
          break;
        case "dlna_append":
          fetch(`http://${hostname}/dlna/append?objectId=${encodeURIComponent(dlnaObjectIdParam())}&limit=${dlnaParseLimit()}`).then(function(r){ return r.json(); }).then(function(j){
            dlnaToast(j.queued ? 'DLNA: append в очереди' : 'DLNA: append не поставлен', !j.queued);
          }).catch(function(){ dlnaToast('DLNA append: ошибка сети', true); });
          break;
        case "dlna_use_web":
          fetch(`http://${hostname}/playlist/web`).then(function(){
            dlnaToast('Источник плейлиста: WEB', false);
            dlnaAfterSourceSwitch();
          }).catch(function(){ dlnaToast('playlist/web: ошибка сети', true); });
          break;
        case "dlna_use_dlna":
          fetch(`http://${hostname}/playlist/dlna`).then(function(){
            dlnaToast('Источник плейлиста: DLNA', false);
            dlnaAfterSourceSwitch();
          }).catch(function(){ dlnaToast('playlist/dlna: ошибка сети', true); });
          break;
          
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
    // После OTA "слепой" redirect часто попадает в окно, когда ESP ещё не поднял HTTP.
    // Из-за этого пользователь видел страницу ошибки и жал F5 вручную.
    // Переходим только после подтверждения готовности web-сервера.
    waitForWebReadyAndRedirect('/', 0);
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

  // Провайдеры: Gemini=0, DeepSeek=1, iTunes=2, LastFM=3, Groq=4
  if (val == 2) {
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
    if (val == 4) {
      span.innerHTML = 'groq api key [<a href="https://console.groq.com/keys" target="_blank">get free key</a>]';
      k.placeholder = "gsk_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
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

// TrackFacts: только facts.js (подключается выше), без второго setInterval — иначе двойной poll /api/current-fact.

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
            for (let i = 0; i <= items.length-1; i++) items[i].getElementsByTagName('span')[0].innerText=String(i+1).padStart(3, '0');
        }
    });
})();
