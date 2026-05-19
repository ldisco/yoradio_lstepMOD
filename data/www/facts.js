// TrackFacts — один экземпляр опроса /api/current-fact (защита от повторной загрузки script.js)
(function() {
    'use strict';

    if (window.__factsInit) {
        console.log('[TrackFacts] Already initialized, skip duplicate');
        return;
    }
    window.__factsInit = true;

    console.log('[TrackFacts] Module loaded v0.4.4');

    const CONFIG = {
        API_ENDPOINT: '/api/current-fact',
        // Базовый (редкий) опрос: при ручном режиме фактов не нужен частый фон.
        CHECK_INTERVAL: 15000,
        // Быстрый опрос только в короткие окна активного запроса/открытого диалога.
        FAST_CHECK_INTERVAL: 3000,
        // Таймаут fetch: не даём зависшему HTTP-запросу держать цикл опроса.
        FETCH_TIMEOUT_MS: 3000,
        META_MAX_INLINE_FACTS: 2,
        META_VISIBLE_MS: 35000
    };

    let lastTitle = '';
    let lastRawFacts = '';
    let currentFacts = [];
    // Таймаут адаптивного опроса facts API (вместо постоянного setInterval).
    let factsPollTimerId = null;
    /** Сохраняем tfprovider из /api/current-fact для повторного рендера без свежего poll. */
    let lastTfProvider = 2;
    /** null = старая прошивка без factsAi; true/false с поля JSON factsAi. */
    let lastFactsAi = null;

    /**
     * Дисклеймер под фактами: только если текст реально от LLM (factsAi с прошивки).
     * Эвристика: строки iTunes:/Last.fm — без дисклеймера; без factsAi — fallback по tfprovider (0/1/4).
     */
    window.trackFactsAiDisclaimerHtml = function(tfprovider, factsAi, factsArr) {
        var looksCatalog = false;
        if (factsArr && factsArr.length) {
            for (var i = 0; i < factsArr.length; i++) {
                var s = String(factsArr[i] || '').trim();
                if (s.indexOf('iTunes:') === 0 || s.indexOf('Last.fm') === 0) {
                    looksCatalog = true;
                    break;
                }
            }
        }
        if (looksCatalog) return '';
        var n = tfprovider != null ? Number(tfprovider) : 2;
        var showAi = false;
        if (factsAi === true || factsAi === 1) {
            showAi = true;
        } else if (factsAi === false || factsAi === 0) {
            showAi = false;
        } else {
            showAi = (n === 0 || n === 1 || n === 4);
        }
        if (!showAi) return '';
        return '<div class="track-fact-ai-note" style="opacity:0.72;font-size:0.82em;margin-top:0.35em;line-height:1.25">Текст сгенерирован ИИ; возможны неточности.</div>';
    };

    function teardownFactsTimers() {
        if (window.__factsCycleTimerId != null) {
            clearTimeout(window.__factsCycleTimerId);
            window.__factsCycleTimerId = null;
        }
        if (window.__factsCheckIntervalId != null) {
            clearTimeout(window.__factsCheckIntervalId);
            window.__factsCheckIntervalId = null;
        }
        if (factsPollTimerId != null) {
            clearTimeout(factsPollTimerId);
            factsPollTimerId = null;
        }
        if (window.__metaFactsHideTimerId != null) {
            clearTimeout(window.__metaFactsHideTimerId);
            window.__metaFactsHideTimerId = null;
        }
    }

    function hideMetaFactsOverlay() {
        window.__metaFactsHideTimerId = null;
        const meta = getMetaElement();
        if (!meta) return;
        if (!meta.classList.contains('track-fact')) return;
        meta.classList.remove('track-fact');
        meta.classList.remove('track-fact-stack');
        meta.style.fontSize = '';
        meta.style.color = '';
        meta.style.fontStyle = '';
        meta.style.lineHeight = '';
        meta.style.textAlign = '';
        meta.innerHTML = '';
        var t = stripFactsHourglass(lastTitle || '');
        if (t && !isSystemMetaValue(t) && isPlayerPlaying()) {
            meta.textContent = t;
        }
    }

    function scheduleMetaFactsHide() {
        if (window.__metaFactsHideTimerId != null) {
            clearTimeout(window.__metaFactsHideTimerId);
        }
        window.__metaFactsHideTimerId = setTimeout(hideMetaFactsOverlay, CONFIG.META_VISIBLE_MS);
    }

    function isPlayerPlaying() {
        const playerwrap = document.getElementById('playerwrap');
        if (playerwrap) return playerwrap.classList.contains('playing');
        return false;
    }

    function getMetaElement() {
        return document.getElementById('meta');
    }

    function isSystemMetaValue(value) {
        if (!value) return false;
        if (value.startsWith('[')) return true;
        if (value.indexOf('Индексация SD') !== -1) return true;
        if (value.indexOf('Загрузка плейлиста') !== -1) return true;
        if (value.indexOf('Переподключение') !== -1) return true;
        return false;
    }

    /** Пока плеер «playing» в DOM, не затираем #meta, если там уже служебная строка с ESP (WS). */
    function factsMayOverwriteMetaFromApi() {
        if (!isPlayerPlaying()) return false;
        var m = getMetaElement();
        if (!m) return false;
        if (isSystemMetaValue((m.textContent || '').trim())) return false;
        return true;
    }

    function stripFactsHourglass(title) {
        if (!title) return '';
        var t = String(title);
        if (t.endsWith(' \u23F3')) t = t.slice(0, -2);
        else if (t.endsWith('\u23F3')) t = t.slice(0, -1);
        return t.trimEnd();
    }

    /**
     * Определяем, нужно ли показывать "⏳" рядом с заголовком трека.
     * Логика: только во время реального запроса к провайдеру (requestPending),
     * а не в состоянии "серия ещё не добрана".
     */
    function isRequestPendingOnly(data) {
        if (!data) return false;
        if (data.requestPending !== undefined && data.requestPending !== null) {
            return Number(data.requestPending) === 1 || data.requestPending === true;
        }
        // Fallback для старой прошивки без requestPending:
        // оставляем прежнее поведение.
        return Number(data.pending) === 1 || data.pending === true;
    }

    function escapeFactLine(s) {
        return String(s)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    /** До META_MAX_INLINE_FACTS под строкой трека; 3-й и дальше — только в диалоге. */
    function renderMetaFactsStack(factsAll, tfprovider) {
        const meta = getMetaElement();
        if (!meta || window.factsDialogOpen) return;
        if (!factsAll || factsAll.length === 0) return;
        if (isSystemMetaValue((meta.textContent || '').trim())) return;
        var titleLine = stripFactsHourglass(lastTitle || '');
        if (isSystemMetaValue(titleLine)) return;
        var slice = factsAll.slice(0, CONFIG.META_MAX_INLINE_FACTS);
        if (slice.length === 0) return;
        meta.classList.add('track-fact');
        meta.classList.add('track-fact-stack');
        meta.style.fontSize = '1.05em';
        meta.style.color = '';
        meta.style.fontStyle = '';
        meta.style.lineHeight = '1.35';
        meta.style.textAlign = 'center';
        var titleHtml = titleLine.length > 0
            ? '<div class="meta-inline-title">' + escapeFactLine(titleLine) + '</div>'
            : '';
        var factsHtml = slice.map(function(f) {
            return '<div class="track-fact-row">💡 ' + escapeFactLine(f.trim()) + '</div>';
        }).join('');
        var disc = (typeof window.trackFactsAiDisclaimerHtml === 'function')
            ? window.trackFactsAiDisclaimerHtml(tfprovider != null ? tfprovider : lastTfProvider, lastFactsAi, factsAll)
            : '';
        //meta.innerHTML = titleHtml + factsHtml + disc;
        meta.innerHTML = titleHtml + factsHtml;
        scheduleMetaFactsHide();
    }

    function resetAll() {
        if (window.__factsCycleTimerId != null) {
            clearTimeout(window.__factsCycleTimerId);
            window.__factsCycleTimerId = null;
        }
        if (window.__metaFactsHideTimerId != null) {
            clearTimeout(window.__metaFactsHideTimerId);
            window.__metaFactsHideTimerId = null;
        }
        currentFacts = [];
        lastRawFacts = '';
        const meta = getMetaElement();
        if (meta) {
            // До очистки: служебные строки WebSocket ([остановлено], [готов], …) — не трогаем.
            var prevTxt = (meta.textContent || '').trim();
            var preserveSystem = isSystemMetaValue(prevTxt);
            meta.classList.remove('track-fact');
            meta.classList.remove('track-fact-stack');
            meta.style.fontSize = '';
            meta.style.color = '';
            meta.style.fontStyle = '';
            meta.style.lineHeight = '';
            meta.style.textAlign = '';
            meta.innerHTML = '';
            if (preserveSystem) {
                meta.textContent = prevTxt;
            } else if (isPlayerPlaying() && lastTitle && !isSystemMetaValue(lastTitle)) {
                meta.textContent = lastTitle;
            } else if (isPlayerPlaying() && prevTxt) {
                // Не оставляем #meta пустым при сбросе facts-состояния:
                // если текущий текст уже есть (например из WS или station fallback),
                // сохраняем его до прихода следующего корректного TITLE.
                meta.textContent = prevTxt;
            }
        }
    }

    window.resetTrackFacts = function() {
        lastTitle = '';
        lastTfProvider = 2;
        lastFactsAi = null;
        if (typeof setTrackFactsCache === 'function') setTrackFactsCache('', []);
        resetAll();
        console.log('[TrackFacts] Reset by mode change');
    };

    let pollInFlight = false;
    function scheduleFactsPoll(delayMs) {
        if (factsPollTimerId != null) {
            clearTimeout(factsPollTimerId);
            factsPollTimerId = null;
        }
        window.__factsCheckIntervalId = null;
        const d = (typeof delayMs === 'number' && delayMs > 0) ? delayMs : CONFIG.CHECK_INTERVAL;
        factsPollTimerId = setTimeout(function() {
            factsPollTimerId = null;
            checkAndDisplayFact();
        }, d);
        window.__factsCheckIntervalId = factsPollTimerId;
    }

    async function checkAndDisplayFact() {
        var nextDelayMs = CONFIG.CHECK_INTERVAL;
        // Если TrackFacts выключен в настройках, не создаём лишнюю нагрузку на веб/API.
        if (typeof window.trackFactsEnabled !== 'undefined' && !window.trackFactsEnabled) {
            resetAll();
            scheduleFactsPoll(30000);
            return;
        }
        if (!isPlayerPlaying()) {
            resetAll();
            scheduleFactsPoll(nextDelayMs);
            return;
        }
        if (pollInFlight) {
            scheduleFactsPoll(CONFIG.FAST_CHECK_INTERVAL);
            return;
        }
        pollInFlight = true;
        try {
            const ac = (typeof AbortController !== 'undefined') ? new AbortController() : null;
            let abortTimer = null;
            if (ac) {
                abortTimer = setTimeout(function() {
                    try { ac.abort(); } catch (e) {}
                }, CONFIG.FETCH_TIMEOUT_MS);
            }
            const response = await fetch(
                CONFIG.API_ENDPOINT + '?t=' + Date.now(),
                ac ? { signal: ac.signal } : undefined
            );
            if (abortTimer) clearTimeout(abortTimer);
            if (!response.ok) {
                scheduleFactsPoll(nextDelayMs);
                return;
            }

            const data = await response.json();
            // Пока ждали fetch, пользователь мог нажать Stop: не подставлять старый title с API поверх [остановлено] и т.д.
            if (!isPlayerPlaying()) {
                scheduleFactsPoll(nextDelayMs);
                return;
            }
            if (!data || !data.title) {
                scheduleFactsPoll(nextDelayMs);
                return;
            }

            if (data.tfprovider !== undefined && data.tfprovider !== null) {
                lastTfProvider = Number(data.tfprovider);
            }
            if (data.factsAi !== undefined && data.factsAi !== null) {
                lastFactsAi = Number(data.factsAi) === 1;
            } else {
                lastFactsAi = null;
            }

            if (data.title !== lastTitle) {
                lastTitle = stripFactsHourglass(data.title);
                resetAll();

                const facts = (data.facts || []).filter(function(f) { return f && f.trim().length > 0; });
                lastRawFacts = facts.join('###');

                if (facts.length > 0) {
                    currentFacts = facts;
                    renderMetaFactsStack(facts, lastTfProvider);
                }
            } else {
                const facts = (data.facts || []).filter(function(f) { return f && f.trim().length > 0; });
                const newRawFacts = facts.join('###');
                lastTitle = stripFactsHourglass(data.title);
                if (newRawFacts !== lastRawFacts && newRawFacts.length > 0) {
                    lastRawFacts = newRawFacts;
                    currentFacts = facts;
                    renderMetaFactsStack(facts, lastTfProvider);
                }
                if (facts.length === 0 && !data.pending) {
                    resetAll();
                }
            }

            if (window.factsDialogOpen) {
                const meta = getMetaElement();
                if (meta && lastTitle && !isSystemMetaValue(lastTitle) && factsMayOverwriteMetaFromApi()) {
                    meta.classList.remove('track-fact');
                    meta.classList.remove('track-fact-stack');
                    meta.style.fontSize = '';
                    meta.style.color = '';
                    meta.style.fontStyle = '';
                    meta.style.lineHeight = '';
                    meta.style.textAlign = '';
                    var pendingNow = isRequestPendingOnly(data);
                    meta.textContent = pendingNow ? lastTitle + ' \u23F3' : lastTitle;
                }
            } else if (currentFacts.length > 0) {
                renderMetaFactsStack(currentFacts, lastTfProvider);
            } else if (factsMayOverwriteMetaFromApi()) {
                const meta = getMetaElement();
                if (meta) {
                    meta.classList.remove('track-fact');
                    meta.classList.remove('track-fact-stack');
                    meta.style.fontSize = '';
                    meta.style.color = '';
                    meta.style.fontStyle = '';
                    meta.style.lineHeight = '';
                    meta.style.textAlign = '';
                    meta.innerHTML = '';
                    if (isRequestPendingOnly(data)) {
                        meta.textContent = lastTitle + ' \u23F3';
                    } else {
                        meta.textContent = lastTitle;
                    }
                }
            }
            // Быстрый polling нужен только при реальной активности TrackFacts:
            // - идёт сетевой запрос к провайдеру;
            // - открыт диалог фактов.
            const requestPendingNow = Number(data.requestPending) === 1 || data.requestPending === true;
            const fastMode = window.factsDialogOpen || requestPendingNow;
            nextDelayMs = fastMode ? CONFIG.FAST_CHECK_INTERVAL : CONFIG.CHECK_INTERVAL;
        } catch (error) {
            console.error('[TrackFacts] Error:', error);
        } finally {
            pollInFlight = false;
            scheduleFactsPoll(nextDelayMs);
        }
    }

    function init() {
        teardownFactsTimers();
        checkAndDisplayFact();
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
