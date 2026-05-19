(function() {
    'use strict';

    // Редкий опрос ESP: мета меняется через WS; #meta без лишних дерганий (см. observer).
    const CHECK_INTERVAL = 45000;
    const DEFAULT_LOGO = '/logo.svg';
    const COVER_HEIGHT = '160px';
    const COVER_MAX_WIDTH = '250px';

    let currentCoverUrl = '';
    let lastProcessedNormTitle = '';
    let coverFetchInProgress = false;
    let isModeSwitching = false;

    let metaObserver = null;
    let lastNamesetSnapshot = '';
    let lastMetaSnapshot = '';
    var lastCoverRelevantMetaNorm = '';
    var lastCoverVisKey = null;

    let itunesInFlightKey = '';
    let itunesInFlightPromise = null;
    // Антифликер: новая мета должна немного "устояться", прежде чем менять обложку.
    let pendingNormTitle = '';
    let pendingNormSinceMs = 0;
    // Окно антидребезга заголовка для смены обложки.
    // Держим умеренным, чтобы не задерживать показ актуальной картинки.
    const TITLE_SWITCH_STABLE_MS = 2200;

    /** Прямой URL с ESP (icy) иногда рвётся (NET_RESET, hotlink) — не крутим его по кругу, уходим в iTunes. */
    var blockedExternalCoverUrl = '';
    var lastServerCoverTitle = '';
    /** Один повторный опрос iTunes при blocked + лого (не долбить API каждые 45 с). */
    var blockedItunesRetryDoneKey = '';

    function teardownCoverTimers() {
        if (window.__coverPollIntervalId != null) {
            clearInterval(window.__coverPollIntervalId);
            window.__coverPollIntervalId = null;
        }
        if (window.__coverClickTimerId != null) {
            clearTimeout(window.__coverClickTimerId);
            window.__coverClickTimerId = null;
        }
        if (window.__coverMutationTimerId != null) {
            clearTimeout(window.__coverMutationTimerId);
            window.__coverMutationTimerId = null;
        }
        if (window.__coverKickTimerId != null) {
            clearTimeout(window.__coverKickTimerId);
            window.__coverKickTimerId = null;
        }
    }

    function normalizeMetaForCover(s) {
        if (!s) return '';
        return s.replace(/\s*⏳\s*$/u, '').trim();
    }
    function isSystemMetaValue(v) {
        if (!v) return false;
        var s = String(v).trim();
        if (!s) return false;
        if (s.charAt(0) === '[') return true;
        if (s.indexOf('Индексация SD') >= 0) return true;
        if (s.indexOf('Загрузка плейлиста') >= 0) return true;
        if (s.indexOf('Переподключение') >= 0) return true;
        return false;
    }

    /** Аналог cleanTrackPrefix (config.cpp) — снять "11 | ", "5. " и т.д. */
    function cleanTrackPrefixJs(title) {
        var cleaned = String(title || '').trim();
        var i = 0;
        while (i < cleaned.length && cleaned.charAt(i) >= '0' && cleaned.charAt(i) <= '9') i++;
        if (i > 0 && i < cleaned.length) {
            while (i < cleaned.length && cleaned.charAt(i) === ' ') i++;
            if (i < cleaned.length) {
                var c = cleaned.charAt(i);
                if (c === '|' || c === '-' || c === '.' || c === ':') {
                    i++;
                    while (i < cleaned.length && cleaned.charAt(i) === ' ') i++;
                    cleaned = cleaned.substring(i).trim();
                }
            }
        }
        return cleaned;
    }

    /**
     * Аналог normalizeTitleForCoverSearch (config.cpp): ipmusic.ch, технические скобки в хвосте.
     * Без этого iTunes ищет по «Adrian Gurvitz - Classic *** www.ipmusic.ch» и не находит обложку.
     */
    function normalizeTitleForCoverSearchJs(title) {
        var normalized = cleanTrackPrefixJs(normalizeMetaForCover(title));
        normalized = normalized.replace(/[\s\u00a0]+/g, ' ').trim();

        var normalizedLower = normalized.toLowerCase();
        var ipIdx = normalizedLower.indexOf('ipmusic.ch');
        if (ipIdx >= 0) {
            var starPos = normalizedLower.lastIndexOf('***', ipIdx);
            var wwwPos = normalizedLower.lastIndexOf('www.', ipIdx);
            var cutPos = starPos >= 0 ? starPos : (wwwPos >= 0 ? wwwPos : ipIdx);
            normalized = normalized.substring(0, cutPos).trim();
            normalizedLower = normalized.toLowerCase();
        }

        while (normalized.endsWith(')')) {
            var openPos = normalized.lastIndexOf('(');
            if (openPos < 0) break;
            var tail = normalized.substring(openPos);
            if (tail.length > 48) break;
            var tailLower = tail.toLowerCase();
            var looksTechnical =
                tailLower.indexOf('.cut') >= 0 ||
                tailLower.indexOf('cdj') >= 0 ||
                tailLower.indexOf('djshaman') >= 0 ||
                tailLower.indexOf('stream') >= 0 ||
                tailLower.indexOf('record') >= 0 ||
                tailLower.indexOf('rip') >= 0 ||
                tailLower.indexOf('www.') >= 0 ||
                tailLower.indexOf('*** www.ipmusic.ch') >= 0 ||
                tailLower.indexOf('http') >= 0;
            if (!looksTechnical) break;
            normalized = normalized.substring(0, openPos).trim();
        }

        // Запасной срез: любой хвост с ipmusic (другие звёздочки/кодировки меты).
        var il = normalized.toLowerCase();
        var ixMusic = il.indexOf('ipmusic');
        if (ixMusic >= 0) {
            normalized = normalized.substring(0, ixMusic).replace(/[\s*·.]+$/g, '').trim();
        }

        return normalized;
    }

    /** Строка запроса iTunes — согласована с прошивкой (ipmusic и служебные хвосты). */
    function itunesSearchQueryFromTitle(songTitle) {
        var q = normalizeMetaForCover(songTitle || '');
        q = q.replace(/\[.*?\]|\(.*?\)/g, '').trim();
        q = normalizeTitleForCoverSearchJs(q);
        return q;
    }

    function isCoverSearchBlockedTitle(t) {
        if (!t || t.length < 3) return true;
        return /timeout/i.test(t);
    }

    function normalizeNameForCompare(s) {
        return String(s || '').replace(/\s+/g, ' ').trim().toLowerCase();
    }

    /** Есть нормальная мета трека — можно искать обложку в iTunes вместо лого/картинки станции. */
    function looksLikeRealTrack(currentTitle, stationName) {
        var raw = String(currentTitle || '').trim();
        if (raw.length < 3) return false;
        if (raw.charAt(0) === '[') return false;
        var normTitle = itunesSearchQueryFromTitle(raw);
        if (isCoverSearchBlockedTitle(normTitle)) return false;
        var st = normalizeNameForCompare(stationName);
        if (st) {
            if (normalizeNameForCompare(raw) === st) return false;
            if (normalizeNameForCompare(normTitle) === st) return false;
        }
        return true;
    }

    /** Прямая ссылка из ICY на бренд станции (не кэш ESP / не iTunes). При реальном треке — iTunes. */
    function looksLikeStationBrandingUrl(url) {
        if (!url || typeof url !== 'string') return false;
        if (url === 'SEARCH_ITUNES') return false;
        var u = url.toLowerCase();
        if (u.indexOf('mzstatic.com') >= 0) return false;
        if (u.indexOf('/sc/cache/') >= 0) return false;
        if (u.indexOf('http://') === 0 || u.indexOf('https://') === 0) return true;
        if (u.indexOf('//') === 0) return true;
        // Тот же хост, что и UI: station_covers, icy с /path/… — не кэш трека из /sc/cache/.
        if (u.charAt(0) === '/' && u.indexOf('logo.svg') < 0) return true;
        return false;
    }

    /**
     * metaTitle на ESP обновляется с debounce; #meta в браузере часто опережает /api/current-cover.
     * Если в DOM уже нормальная мета трека — используем её для iTunes, иначе title из API.
     */
    function resolveTitleForCover(apiTitle, stationName) {
        var apiT = normalizeMetaForCover(apiTitle || '');
        var domEl = document.getElementById('meta');
        var domRaw = domEl ? String(domEl.textContent || '').trim() : '';
        if (domRaw.indexOf('💡') === 0) {
            domRaw = '';
        }
        var domT = normalizeMetaForCover(domRaw);
        if (looksLikeRealTrack(domT, stationName)) {
            return domT;
        }
        return apiT;
    }

    function itunesSearchCandidatesFromTitle(songTitle) {
        var primary = itunesSearchQueryFromTitle(songTitle);
        var out = [];
        function pushUnique(s) {
            s = String(s || '').trim();
            if (s.length < 3 || isCoverSearchBlockedTitle(s)) return;
            for (var i = 0; i < out.length; i++) {
                if (out[i] === s) return;
            }
            out.push(s);
        }
        pushUnique(primary);
        var dash = primary.indexOf(' - ');
        if (dash > 0) {
            pushUnique(primary.substring(0, dash));
            pushUnique(primary.substring(dash + 3));
        }
        return out;
    }

    async function fetchiTunesArtworkForTerm(term) {
        var url = 'https://itunes.apple.com/search?term=' + encodeURIComponent(term) +
            '&entity=musicTrack&limit=5';
        var response = await fetch(url);
        var data = await response.json();
        if (data.results && data.results.length > 0) {
            return data.results[0].artworkUrl100.replace('100x100bb.jpg', '600x600bb.jpg');
        }
        return '';
    }

    async function fetchiTunesCover(songTitle) {
        if (isModeSwitching) return DEFAULT_LOGO;
        var candidates = itunesSearchCandidatesFromTitle(songTitle);
        if (!candidates.length) return DEFAULT_LOGO;

        var dedupeKey = candidates.join('\n');
        if (itunesInFlightPromise && itunesInFlightKey === dedupeKey) {
            return itunesInFlightPromise;
        }

        itunesInFlightKey = dedupeKey;
        itunesInFlightPromise = (async function() {
            try {
                for (var c = 0; c < candidates.length; c++) {
                    var art = await fetchiTunesArtworkForTerm(candidates[c]);
                    if (art) return art;
                }
            } catch (e) {
                console.error('[iTunes] Error:', e);
            }
            return DEFAULT_LOGO;
        })();

        try {
            return await itunesInFlightPromise;
        } finally {
            itunesInFlightPromise = null;
            itunesInFlightKey = '';
        }
    }

    function isPlayerPlaying() {
        var playerwrap = document.getElementById('playerwrap');
        if (playerwrap) return playerwrap.classList.contains('playing');
        var modeplaying = document.getElementById('modeplaying');
        if (!modeplaying) return false;
        if (modeplaying.classList.contains('hidden')) return false;
        if (modeplaying.style && modeplaying.style.display === 'none') return false;
        return true;
    }

    /** После сбоя прямой картинки (onerror или load с нулевым размером) — iTunes по мете этого запроса. */
    function scheduleItunesFallbackForFailedUrl(failedCoverUrl, metaTitleSnap) {
        if (!metaTitleSnap || metaTitleSnap.length < 3 || metaTitleSnap.charAt(0) === '[') return false;
        if (isCoverSearchBlockedTitle(itunesSearchQueryFromTitle(metaTitleSnap))) return false;
        var failed = failedCoverUrl;
        if (!(failed.indexOf('http://') === 0 || failed.indexOf('https://') === 0)) return false;
        if (failed.indexOf('mzstatic.com') >= 0) return false;
        blockedExternalCoverUrl = failed;
        setTimeout(function() {
            if (!isPlayerPlaying() || isModeSwitching) return;
            fetchiTunesCover(metaTitleSnap).then(function(iu) {
                currentCoverUrl = iu;
                lastProcessedNormTitle = itunesSearchQueryFromTitle(metaTitleSnap);
                updateLogoDisplay(iu, true, metaTitleSnap);
            }).catch(function() {
                lastCoverVisKey = null;
                updateLogoDisplay(DEFAULT_LOGO, true, metaTitleSnap);
            });
        }, 0);
        return true;
    }

    function updateLogoDisplay(coverUrl, isPlaying, metaForCoverFallback) {
        var logoDiv = document.getElementById('logo');
        var coverDiv = document.getElementById('cover-art-display');
        if (!logoDiv || !coverDiv) return;

        var titleForFallback = (typeof metaForCoverFallback === 'string' && metaForCoverFallback.length > 0)
            ? metaForCoverFallback
            : lastServerCoverTitle;

        var effective = (!coverUrl || coverUrl.length < 5) ? DEFAULT_LOGO : coverUrl;
        var showAsLogo = !isPlaying || effective.indexOf('logo.svg') >= 0;
        var visKey = showAsLogo ? ('logo:' + (isPlaying ? '1' : '0')) : (effective + '|' + (isPlaying ? '1' : '0'));
        if (visKey === lastCoverVisKey) return;
        lastCoverVisKey = visKey;

        var commonStyle = 'display:flex; justify-content:center; align-items:center; flex:1; width:100%; min-width:0; margin:0 auto;';

        var showLogo = function() {
            coverDiv.style.display = 'none';
            coverDiv.innerHTML = '';
            logoDiv.style.cssText = commonStyle;
            logoDiv.style.display = 'flex';
        };

        if (!coverUrl || coverUrl.length < 5) {
            showLogo();
            return;
        }

        if (showAsLogo) {
            showLogo();
        } else {
            var img = document.createElement('img');
            var h = (typeof COVER_HEIGHT !== 'undefined') ? COVER_HEIGHT : '100%';
            var w = (typeof COVER_MAX_WIDTH !== 'undefined') ? COVER_MAX_WIDTH : '100%';
            img.style.cssText = 'height:' + h + '; width:auto; max-width:' + w + '; object-fit:contain; border-radius:9px; display:block;';
            if (coverUrl.indexOf('https://') === 0 || coverUrl.indexOf('http://') === 0) {
                img.referrerPolicy = 'no-referrer';
            }

            img.onload = function() {
                var nw = this.naturalWidth || this.width;
                var nh = this.naturalHeight || this.height;
                if (nw > 0 && nh > 0) {
                    coverDiv.innerHTML = '';
                    coverDiv.appendChild(this);
                    coverDiv.style.cssText = commonStyle;
                    coverDiv.style.display = 'flex';
                    logoDiv.style.display = 'none';
                } else {
                    lastCoverVisKey = null;
                    if (!scheduleItunesFallbackForFailedUrl(coverUrl, titleForFallback)) {
                        showLogo();
                    }
                }
            };

            img.onerror = function() {
                lastCoverVisKey = null;
                if (!scheduleItunesFallbackForFailedUrl(coverUrl, titleForFallback)) {
                    showLogo();
                }
            };

            img.src = coverUrl;
        }
    }

    async function fetchCoverFromServer() {
        try {
            var response = await fetch('/api/current-cover?t=' + Date.now());
            if (response.ok) {
                return await response.json();
            }
        } catch (e) {
            console.error('[CoverArt] Server error');
        }
        return { url: DEFAULT_LOGO };
    }

    async function checkAndUpdateCover() {
        if (isModeSwitching) return;
        if (window.location.hash.indexOf('playlist') >= 0 || document.getElementById('playlisteditor')) {
            return;
        }
        var isNowPlaying = isPlayerPlaying();
        var serverData = await fetchCoverFromServer();
        if (!serverData) return;

        var serverUrl = serverData.url || DEFAULT_LOGO;
        var stationName = (serverData && serverData.station) ? serverData.station : '';
        var coverTitle = resolveTitleForCover(serverData.title || '', stationName);
        var normTitle = itunesSearchQueryFromTitle(coverTitle);
        lastServerCoverTitle = coverTitle;

        if (blockedExternalCoverUrl && serverUrl !== blockedExternalCoverUrl) {
            blockedExternalCoverUrl = '';
        }

        if (!isNowPlaying) {
            currentCoverUrl = DEFAULT_LOGO;
            lastProcessedNormTitle = '';
            coverFetchInProgress = false;
            blockedExternalCoverUrl = '';
            blockedItunesRetryDoneKey = '';
            updateLogoDisplay(DEFAULT_LOGO, false);
            return;
        }

        var wantsTrack = looksLikeRealTrack(coverTitle, stationName);
        var useItunesBranch =
            serverUrl === 'SEARCH_ITUNES' ||
            (blockedExternalCoverUrl && serverUrl === blockedExternalCoverUrl) ||
            (wantsTrack && serverUrl.indexOf('station_covers/') >= 0) ||
            (wantsTrack && serverUrl.indexOf('logo.svg') >= 0) ||
            (wantsTrack && looksLikeStationBrandingUrl(serverUrl));

        if (useItunesBranch) {
            if (isModeSwitching || coverTitle.length < 3 || coverTitle.charAt(0) === '[') return;
            if (isCoverSearchBlockedTitle(normTitle)) {
                currentCoverUrl = DEFAULT_LOGO;
                updateLogoDisplay(DEFAULT_LOGO, isNowPlaying, coverTitle);
                return;
            }

            if (lastProcessedNormTitle !== normTitle) {
                // Часто поток шлёт промежуточные/служебные варианты title, из-за чего
                // iTunes успевает отдать разные картинки для соседних строк.
                // Если уже показана валидная обложка, требуем краткую стабильность новой меты.
                var hasRealCoverNow =
                    !!currentCoverUrl &&
                    currentCoverUrl !== DEFAULT_LOGO &&
                    currentCoverUrl.indexOf('logo.svg') < 0;
                if (hasRealCoverNow && lastProcessedNormTitle.length > 0) {
                    var nowMs = Date.now();
                    if (pendingNormTitle !== normTitle) {
                        pendingNormTitle = normTitle;
                        pendingNormSinceMs = nowMs;
                        // Не держим старую "чужую" обложку при старте новой станции:
                        // на период стабилизации показываем логотип.
                        currentCoverUrl = DEFAULT_LOGO;
                        updateLogoDisplay(DEFAULT_LOGO, true, coverTitle);
                        return;
                    }
                    if (nowMs - pendingNormSinceMs < TITLE_SWITCH_STABLE_MS) {
                        return;
                    }
                }
                blockedItunesRetryDoneKey = '';
                pendingNormTitle = '';
                pendingNormSinceMs = 0;
                lastProcessedNormTitle = normTitle;
                if (coverFetchInProgress || isModeSwitching) return;
                coverFetchInProgress = true;
                try {
                    var itunesUrl = await fetchiTunesCover(coverTitle);
                    currentCoverUrl = itunesUrl;
                    updateLogoDisplay(itunesUrl, true, coverTitle);
                } finally {
                    coverFetchInProgress = false;
                }
            } else if (blockedExternalCoverUrl && serverUrl === blockedExternalCoverUrl &&
                blockedItunesRetryDoneKey !== normTitle &&
                (currentCoverUrl === DEFAULT_LOGO || (currentCoverUrl && currentCoverUrl.indexOf('logo.svg') >= 0)) &&
                !coverFetchInProgress && !isModeSwitching) {
                blockedItunesRetryDoneKey = normTitle;
                coverFetchInProgress = true;
                try {
                    var itunesRetry = await fetchiTunesCover(coverTitle);
                    currentCoverUrl = itunesRetry;
                    updateLogoDisplay(itunesRetry, true, coverTitle);
                } finally {
                    coverFetchInProgress = false;
                }
            }
        } else if (serverUrl.indexOf('logo.svg') >= 0) {
            currentCoverUrl = DEFAULT_LOGO;
            lastProcessedNormTitle = '';
            coverFetchInProgress = false;
            blockedExternalCoverUrl = '';
            blockedItunesRetryDoneKey = '';
            updateLogoDisplay(DEFAULT_LOGO, true, coverTitle);
        } else if (serverUrl !== currentCoverUrl) {
            // Для прямых URL от сервера применяем ту же антидребезг-логику, что и для iTunes:
            // если уже есть валидная обложка, не переключаемся мгновенно на "соседний" URL.
            var hasRealCoverNowDirect =
                !!currentCoverUrl &&
                currentCoverUrl !== DEFAULT_LOGO &&
                currentCoverUrl.indexOf('logo.svg') < 0;
            if (hasRealCoverNowDirect && lastProcessedNormTitle.length > 0) {
                var nowDirectMs = Date.now();
                if (pendingNormTitle !== normTitle) {
                    pendingNormTitle = normTitle;
                    pendingNormSinceMs = nowDirectMs;
                    // Для прямых URL действуем так же: не показываем старую чужую картинку,
                    // пока новый title не стал стабильным.
                    currentCoverUrl = DEFAULT_LOGO;
                    updateLogoDisplay(DEFAULT_LOGO, true, coverTitle);
                    return;
                }
                if (nowDirectMs - pendingNormSinceMs < TITLE_SWITCH_STABLE_MS) {
                    return;
                }
            }
            pendingNormTitle = '';
            pendingNormSinceMs = 0;
            lastProcessedNormTitle = normTitle;
            coverFetchInProgress = false;
            currentCoverUrl = serverUrl;
            updateLogoDisplay(serverUrl, true, coverTitle);
        }
    }

    function attachMetaNamesetObservers() {
        if (metaObserver) {
            try { metaObserver.disconnect(); } catch (e) {}
            metaObserver = null;
        }
        pendingNormTitle = '';
        pendingNormSinceMs = 0;
        var metaEl = document.getElementById('meta');
        var namesetEl = document.getElementById('nameset');
        if (!metaEl && !namesetEl) return;
        lastNamesetSnapshot = namesetEl ? namesetEl.textContent.trim() : '';
        lastMetaSnapshot = metaEl ? metaEl.textContent.trim() : '';
        var im = lastMetaSnapshot;
        lastCoverRelevantMetaNorm = (im && im.indexOf('💡') !== 0) ? normalizeMetaForCover(im) : '';

        metaObserver = new MutationObserver(function() {
            var n = document.getElementById('nameset');
            var m = document.getElementById('meta');
            var ns = n ? n.textContent.trim() : '';
            var mt = m ? m.textContent.trim() : '';

            if (ns !== lastNamesetSnapshot) {
                // nameset может меняться сам по себе (служебная подпись, перерисовка строки станции),
                // при этом трек остаётся тем же. В таком случае НЕ трогаем текущую обложку,
                // чтобы убрать визуальное "мигание" и лишние запросы.
                var nextMetaNorm = (mt && mt.indexOf('💡') !== 0) ? normalizeMetaForCover(mt) : '';
                var coverRelevantMetaChanged = (nextMetaNorm !== lastCoverRelevantMetaNorm);
                lastNamesetSnapshot = ns;
                lastMetaSnapshot = mt;
                if (!coverRelevantMetaChanged) {
                    return;
                }
                // Если одновременно с nameset изменилась релевантная мета трека —
                // это уже валидный триггер обновления обложки, поэтому сбрасываем
                // промежуточные антидребезг-состояния и запускаем отложенную проверку.
                lastProcessedNormTitle = '';
                coverFetchInProgress = false;
                blockedExternalCoverUrl = '';
                blockedItunesRetryDoneKey = '';
                lastCoverRelevantMetaNorm = nextMetaNorm;
                currentCoverUrl = '';
                lastCoverVisKey = null;
                if (window.__coverMutationTimerId != null) clearTimeout(window.__coverMutationTimerId);
                window.__coverMutationTimerId = setTimeout(function() {
                    window.__coverMutationTimerId = null;
                    checkAndUpdateCover();
                }, 450);
                return;
            }
            // TrackFacts крутит #meta (💡 / название / ⏳) — не дергаем /api/current-cover на каждую смену, только ESP.
            if (mt.indexOf('💡') === 0) {
                lastMetaSnapshot = mt;
                return;
            }
            // Служебные строки статуса не должны сбрасывать обложку/поиск.
            if (isSystemMetaValue(mt)) {
                lastMetaSnapshot = mt;
                return;
            }
            var norm = normalizeMetaForCover(mt);
            if (norm === lastCoverRelevantMetaNorm) {
                lastMetaSnapshot = mt;
                return;
            }
            lastCoverRelevantMetaNorm = norm;
            lastMetaSnapshot = mt;
            lastProcessedNormTitle = '';
            coverFetchInProgress = false;

            if (window.__coverMutationTimerId != null) clearTimeout(window.__coverMutationTimerId);
            window.__coverMutationTimerId = setTimeout(function() {
                window.__coverMutationTimerId = null;
                checkAndUpdateCover();
            }, 600);
        });
        if (metaEl) metaObserver.observe(metaEl, { characterData: true, childList: true, subtree: true });
        if (namesetEl) metaObserver.observe(namesetEl, { characterData: true, childList: true, subtree: true });
    }

    function initCoverSystem() {
        if (window.__coverInit) {
            attachMetaNamesetObservers();
            checkAndUpdateCover();
            return;
        }
        window.__coverInit = true;
        teardownCoverTimers();

        document.addEventListener('modeSwitching', function(e) {
            isModeSwitching = e.detail;
            if (isModeSwitching) {
                currentCoverUrl = DEFAULT_LOGO;
                lastProcessedNormTitle = '';
                coverFetchInProgress = false;
                updateLogoDisplay(DEFAULT_LOGO, false);
            }
        });

        window.__coverPollIntervalId = setInterval(checkAndUpdateCover, CHECK_INTERVAL);
        document.addEventListener('click', function(e) {
            if (e.target.closest('.station') || e.target.closest('#playbutton') || e.target.closest('#modestopped')) {
                if (window.__coverClickTimerId != null) clearTimeout(window.__coverClickTimerId);
                window.__coverClickTimerId = setTimeout(function() {
                    window.__coverClickTimerId = null;
                    checkAndUpdateCover();
                }, 1200);
            }
        });

        attachMetaNamesetObservers();
        checkAndUpdateCover();
        window.__coverKickTimerId = setTimeout(function() {
            window.__coverKickTimerId = null;
            checkAndUpdateCover();
        }, 2000);
        console.log('[CoverArt] System initialized (singleton)');
    }

    window.rebindCoverArtObservers = function() {
        if (!window.__coverInit) {
            initCoverSystem();
            return;
        }
        attachMetaNamesetObservers();
        checkAndUpdateCover();
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initCoverSystem);
    } else {
        initCoverSystem();
    }
})();
