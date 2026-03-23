(function() {
    // Poll fallback (если MutationObserver не сработал) — не чаще чем раз в 30 с.
    // Основной триггер — смена метаданных через MutationObserver на #meta / #nameset.
    const CHECK_INTERVAL = 30000;
    const DEFAULT_LOGO = '/logo.svg';
    const COVER_HEIGHT = '160px';
    const COVER_MAX_WIDTH = '250px';

    let currentCoverUrl = '';
    let isInitialized = false;
    let lastProcessedTitle = '';
    let coverFetchInProgress = false;
    let coverClickTimer = null;
    let coverMutationTimer = null;
    let isModeSwitching = false; // Флаг блокировки при смене режима (Web <-> SD)

    let metaObserver = null;
    let lastNamesetSnapshot = '';
    let lastMetaSnapshot = '';

    // Убираем служебные суффиксы из строки заголовка — иначе «трек + ⏳» и «трек»
    // считаются разными, и обложка моргает при каждом переключении индикатора ожидания.
    function normalizeMetaForCover(s) {
        if (!s) return '';
        return s.replace(/\s*⏳\s*$/u, '').trim();
    }

    // Системные сообщения об ошибках (не название трека) — не дергать iTunes.
    function isCoverSearchBlockedTitle(t) {
        if (!t || t.length < 3) return true;
        return /timeout/i.test(t);
    }

    // Функция поиска обложки в iTunes (когда сервер отдаёт SEARCH_ITUNES)
    async function fetchiTunesCover(songTitle) {
        if (isModeSwitching) return DEFAULT_LOGO;
        if (isCoverSearchBlockedTitle(songTitle)) return DEFAULT_LOGO;
        // Очистка названия: убираем [Ready], (Official Video) и т.д.
        let query = songTitle.replace(/\[.*?\]|\(.*?\)/g, '').trim();
        if (query.length < 3) return DEFAULT_LOGO;

        try {
            const url = `https://itunes.apple.com/search?term=${encodeURIComponent(query)}&entity=musicTrack&limit=1`;
            const response = await fetch(url);
            const data = await response.json();

            if (data.results && data.results.length > 0) {
                // Берём превью и заменяем на 600x600
                return data.results[0].artworkUrl100.replace('100x100bb.jpg', '600x600bb.jpg');
            }
        } catch (e) {
            console.error('[iTunes] Error:', e);
        }
        return DEFAULT_LOGO;
    }

    // 1. Проверка состояния плеера (класс #playerwrap или видимость #modeplaying)
    function isPlayerPlaying() {
        const playerwrap = document.getElementById('playerwrap');
        if (playerwrap) return playerwrap.classList.contains('playing');
        const modeplaying = document.getElementById('modeplaying');
        if (!modeplaying) return false;
        if (modeplaying.classList.contains('hidden')) return false;
        if (modeplaying.style && modeplaying.style.display === 'none') return false;
        return true;
    }

    // 2. Отрисовка: логотип или обложка в #cover-art-display
    function updateLogoDisplay(coverUrl, isPlaying) {
        const logoDiv = document.getElementById('logo');
        const coverDiv = document.getElementById('cover-art-display');
        if (!logoDiv || !coverDiv) return;

        // Единый стиль для центровки
        const commonStyle = 'display:flex; justify-content:center; align-items:center; flex:1; width:100%; min-width:0; margin:0 auto;';

        // Вспомогательная функция: показать логотип, скрыть обложку
        const showLogo = () => {
            coverDiv.style.display = 'none';
            coverDiv.innerHTML = ''; // очищаем обложку, чтобы не мешала
            logoDiv.style.cssText = commonStyle;
            logoDiv.style.display = 'flex';
        };

        // Любой сомнительный URL — только логотип (логотип никуда не «исчезает»).
        if (!coverUrl || coverUrl.length < 5) {
            showLogo();
            return;
        }

        // Показываем логотип по умолчанию, если не играем / нет URL / явный logo.svg
        const isDefault = !isPlaying || !coverUrl || coverUrl.includes('logo.svg');

        if (isDefault) {
            showLogo();
        } else {
            // Обложка: логотип остаётся видимым, пока грузится картинка (onload переключает)
            const img = document.createElement('img');
            const h = (typeof COVER_HEIGHT !== 'undefined') ? COVER_HEIGHT : '100%';
            const w = (typeof COVER_MAX_WIDTH !== 'undefined') ? COVER_MAX_WIDTH : '100%';
            img.style.cssText = `height:${h}; width:auto; max-width:${w}; object-fit:contain; border-radius:9px; display:block;`;

            img.onload = function() {
                if (this.width > 0) {
                    coverDiv.innerHTML = '';
                    coverDiv.appendChild(this);
                    coverDiv.style.cssText = commonStyle;
                    coverDiv.style.display = 'flex';
                    logoDiv.style.display = 'none';
                } else {
                    showLogo();
                }
            };

            img.onerror = function() {
                showLogo();
            };

            img.src = coverUrl;
        }
    }

    // 3. Получение текущей обложки и заголовка с сервера
    async function fetchCoverFromServer() {
        try {
            const response = await fetch('/api/current-cover?t=' + Date.now());
            if (response.ok) {
                return await response.json();
            }
        } catch (e) {
            console.error('[CoverArt] Server error');
        }
        return { url: DEFAULT_LOGO };
    }

    // 4. Опрос API и обновление DOM (плюс ветка SEARCH_ITUNES → клиентский iTunes)
    async function checkAndUpdateCover() {
        if (isModeSwitching) return;
        if (window.location.hash.includes('playlist') || document.getElementById('playlisteditor')) {
            console.log('[CoverArt] Пауза: открыт редактор плейлиста');
            return;
        }
        const isNowPlaying = isPlayerPlaying();
        const serverData = await fetchCoverFromServer();
        if (!serverData) return;

        let serverUrl = serverData.url || DEFAULT_LOGO;
        const currentTitle = serverData.title || '';

        if (!isNowPlaying || serverUrl.includes('logo.svg')) {
            currentCoverUrl = DEFAULT_LOGO;
            lastProcessedTitle = '';
            updateLogoDisplay(DEFAULT_LOGO, false);
        } else if (serverUrl === 'SEARCH_ITUNES') {
            if (isModeSwitching || currentTitle.length < 3 || currentTitle.startsWith('[')) return;
            if (isCoverSearchBlockedTitle(currentTitle)) {
                currentCoverUrl = DEFAULT_LOGO;
                updateLogoDisplay(DEFAULT_LOGO, isNowPlaying);
                return;
            }

            if (lastProcessedTitle !== currentTitle) {
                lastProcessedTitle = currentTitle;
                console.log('[CoverArt] Начинаем поиск в iTunes для:', currentTitle);

                if (currentCoverUrl !== DEFAULT_LOGO) {
                    currentCoverUrl = DEFAULT_LOGO;
                    updateLogoDisplay(DEFAULT_LOGO, true);
                }

                if (coverFetchInProgress || isModeSwitching) return;
                coverFetchInProgress = true;
                const itunesUrl = await fetchiTunesCover(currentTitle);
                currentCoverUrl = itunesUrl;
                updateLogoDisplay(itunesUrl, true);
                coverFetchInProgress = false;
            }
        } else if (serverUrl !== currentCoverUrl) {
            lastProcessedTitle = '';
            coverFetchInProgress = false;
            currentCoverUrl = serverUrl;
            updateLogoDisplay(serverUrl, true);
        }
    }

    // Наблюдатели за #meta и #nameset: сброс «залипшей» обложки при смене станции/трека + отложенный checkAndUpdateCover
    function attachMetaNamesetObservers() {
        if (metaObserver) {
            try { metaObserver.disconnect(); } catch (e) {}
            metaObserver = null;
        }
        const metaEl = document.getElementById('meta');
        const namesetEl = document.getElementById('nameset');
        if (!metaEl && !namesetEl) return;
        lastNamesetSnapshot = namesetEl ? namesetEl.textContent.trim() : '';
        lastMetaSnapshot = metaEl ? metaEl.textContent.trim() : '';

        metaObserver = new MutationObserver(function() {
            const n = document.getElementById('nameset');
            const m = document.getElementById('meta');
            const ns = n ? n.textContent.trim() : '';
            const mt = m ? m.textContent.trim() : '';

            // Смена станции — сразу логотип (как на дисплее), затем отложенный опрос API.
            if (ns !== lastNamesetSnapshot) {
                lastNamesetSnapshot = ns;
                lastProcessedTitle = '';
                coverFetchInProgress = false;
                currentCoverUrl = DEFAULT_LOGO;
                if (isPlayerPlaying()) updateLogoDisplay(DEFAULT_LOGO, true);
            }
            // Смена #meta: без мгновенного сброса картинки (избегаем моргания от ICY / Facts / «⏳»).
            const mtNorm = normalizeMetaForCover(mt);
            const lastNorm = normalizeMetaForCover(lastMetaSnapshot);
            const factOverlay = mt.trim().indexOf('💡') === 0 || (lastMetaSnapshot && lastMetaSnapshot.trim().indexOf('💡') === 0);
            if (!factOverlay && mtNorm !== lastNorm) {
                lastProcessedTitle = '';
                coverFetchInProgress = false;
            }
            lastMetaSnapshot = mt;

            clearTimeout(coverMutationTimer);
            coverMutationTimer = setTimeout(checkAndUpdateCover, 600);
        });
        if (metaEl) metaObserver.observe(metaEl, { characterData: true, childList: true, subtree: true });
        if (namesetEl) metaObserver.observe(namesetEl, { characterData: true, childList: true, subtree: true });
    }

    // 5. Инициализация: интервал, клики по станции/плееру, подписка на modeSwitching из script.js
    function initCoverSystem() {
        if (!isInitialized) {
            document.addEventListener('modeSwitching', function(e) {
                isModeSwitching = e.detail;
                if (isModeSwitching) {
                    console.log('[CoverArt] Блокировка API активна (смена режима)');
                    currentCoverUrl = DEFAULT_LOGO;
                    updateLogoDisplay(DEFAULT_LOGO, false);
                }
            });
            setInterval(checkAndUpdateCover, CHECK_INTERVAL);
            document.addEventListener('click', function(e) {
                if (e.target.closest('.station') || e.target.closest('#playbutton') || e.target.closest('#modestopped')) {
                    clearTimeout(coverClickTimer);
                    coverClickTimer = setTimeout(checkAndUpdateCover, 1200);
                }
            });
            isInitialized = true;
            console.log('[CoverArt] System Initialized');
        }
        attachMetaNamesetObservers();
        checkAndUpdateCover();
        setTimeout(checkAndUpdateCover, 2000);
    }

    // После динамической подгрузки player.html — переподключить observers к новым #meta/#nameset
    window.rebindCoverArtObservers = function() {
        attachMetaNamesetObservers();
        checkAndUpdateCover();
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initCoverSystem);
    } else {
        initCoverSystem();
    }
})();
