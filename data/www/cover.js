(function() {
    // Poll fallback interval (if no title change detected) — reduced load: 60s
    // Увеличено до 60с, так как основной триггер — смена метаданных через MutationObserver
    const CHECK_INTERVAL = 60000; 
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

    // Функция поиска в iTunes
    async function fetchiTunesCover(songTitle) {
        if (isModeSwitching) return DEFAULT_LOGO;
        // Очистка названия: убираем [Ready], (Official Video) и т.д.
        let query = songTitle.replace(/\[.*?\]|\(.*?\)/g, '').trim();
        if (query.length < 3) return DEFAULT_LOGO;

        try {
            const url = `https://itunes.apple.com/search?term=${encodeURIComponent(query)}&entity=musicTrack&limit=1`;
            const response = await fetch(url);
            const data = await response.json();
            
            if (data.results && data.results.length > 0) {
                // Берем картинку и делаем её 600x600
                return data.results[0].artworkUrl100.replace('100x100bb.jpg', '600x600bb.jpg');
            }
        } catch (e) {
            console.error("[iTunes] Error:", e);
        }
        return DEFAULT_LOGO;
    }

    // 1. Проверка состояния плеера
    function isPlayerPlaying() {
        const playerwrap = document.getElementById('playerwrap');
        if (playerwrap) return playerwrap.classList.contains('playing');
        const modeplaying = document.getElementById('modeplaying');
        if (!modeplaying) return false;
        if (modeplaying.classList.contains('hidden')) return false;
        if (modeplaying.style && modeplaying.style.display === 'none') return false;
        return true;
    }

// 2. Отрисовка 
function updateLogoDisplay(coverUrl, isPlaying) {
    if (!coverUrl || coverUrl.length < 5) return;
    const logoDiv = document.getElementById('logo');
    const coverDiv = document.getElementById('cover-art-display');
    if (!logoDiv || !coverDiv) return;

    // Единый стиль для центровки
    const commonStyle = "display:flex; justify-content:center; align-items:center; flex:1; width:100%; min-width:0; margin:0 auto;";
    
    // Вспомогательная функция: Включить Логотип
    const showLogo = () => {
        coverDiv.style.display = 'none';
        coverDiv.innerHTML = ''; // Очищаем обложку, чтобы не мешала
        logoDiv.style.cssText = commonStyle;
        logoDiv.style.display = 'flex';
    };

    // 1. Проверяем, нужно ли показывать Лого по умолчанию
    const isDefault = !isPlaying || !coverUrl || coverUrl.includes('logo.svg');

    if (isDefault) {
        showLogo();
    } else {
        // 2. Логика для Обложки
        // ВНИМАНИЕ: Мы НЕ скрываем логотип здесь. Он висит, пока грузится картинка.
        
        const img = document.createElement('img');
        
        // Используем ваши переменные размера или 100% как запасной вариант
        const h = (typeof COVER_HEIGHT !== 'undefined') ? COVER_HEIGHT : '100%';
        const w = (typeof COVER_MAX_WIDTH !== 'undefined') ? COVER_MAX_WIDTH : '100%';
        
        img.style.cssText = `height:${h}; width:auto; max-width:${w}; object-fit:contain; border-radius:4px; display:block;`;
        
        img.onload = function() {
            // Картинка скачалась. Проверяем, не "битая" ли она (ширина > 0)
            if (this.width > 0) {
                console.log('[CoverArt] Image ready. Swapping logo for cover.');
                
                // Сначала очищаем контейнер и вставляем ЭТУ ЖЕ картинку
                coverDiv.innerHTML = '';
                coverDiv.appendChild(this);
                
                // Включаем контейнер обложки
                coverDiv.style.cssText = commonStyle;
                coverDiv.style.display = 'flex';
                
                // И только ТЕПЕРЬ скрываем логотип
                logoDiv.style.display = 'none';
            } else {
                console.log('[CoverArt] Image loaded but width is 0. Keep Logo.');
                showLogo();
            }
        };

        img.onerror = function() {
            console.log('[CoverArt] Load error. Keep Logo.');
            showLogo();
        };

        // Запускаем процесс (логотип всё это время висит на экране)
        img.src = coverUrl;
    }
}
    // 3. Получение данных с сервера
    async function fetchCoverFromServer() {
        try {
            const response = await fetch('/api/current-cover?t=' + Date.now());
            if (response.ok) {
                return await response.json();
            }
        } catch (e) { 
            console.error("[CoverArt] Server error"); 
        }
        return { url: DEFAULT_LOGO };
    }

    // 4. Логика проверки
  async function checkAndUpdateCover() {
    if (isModeSwitching) return;
    if (window.location.hash.includes('playlist') || document.getElementById('playlisteditor')) {
        console.log("[CoverArt] Пауза: открыт редактор плейлиста");
        return; 
    }
        const isNowPlaying = isPlayerPlaying();
        const serverData = await fetchCoverFromServer();
        if (!serverData) return;
        
        let serverUrl = serverData.url || DEFAULT_LOGO;
        // Теперь берем title прямо из JSON данных сервера
        const currentTitle = serverData.title || ""; 

        if (!isNowPlaying || serverUrl.includes('logo.svg')) {
            // Всегда сбрасываем на логотип при остановке или logo.svg от сервера.
            currentCoverUrl = DEFAULT_LOGO;
            lastProcessedTitle = '';
            updateLogoDisplay(DEFAULT_LOGO, false);
        }
        else if (serverUrl === "SEARCH_ITUNES") {
            // Если в заголовке песни системные сообщения или пусто — не ищем
            if (isModeSwitching || currentTitle.length < 3 || currentTitle.startsWith("[")) return;

            // Если заголовок изменился — сразу показать логотип и начать поиск
            if (lastProcessedTitle !== currentTitle) {
                lastProcessedTitle = currentTitle;
                console.log("[CoverArt] Начинаем поиск в iTunes для:", currentTitle);

                // Показать логотип немедленно, чтобы не держать старую обложку
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
        }
        else if (serverUrl !== currentCoverUrl) {
            // При приходе прямого URL — отменяем возможный поиск и сразу обновляем обложку
            lastProcessedTitle = '';
            coverFetchInProgress = false;
            currentCoverUrl = serverUrl;
            updateLogoDisplay(serverUrl, true);
        }
    }
    // 5. Инициализация
    function initCoverSystem() {
        if (isInitialized) return;
        
        // Слушаем флаг смены режима через кастомное событие из script.js (WebSocket)
        document.addEventListener('modeSwitching', (e) => {
            isModeSwitching = e.detail;
            if (isModeSwitching) {
                console.log("[CoverArt] Блокировка API активна (смена режима)");
                currentCoverUrl = DEFAULT_LOGO;
                updateLogoDisplay(DEFAULT_LOGO, false);
            }
        });

        // Fallback polling
        setInterval(checkAndUpdateCover, CHECK_INTERVAL);

        // React to user clicks (play/stop/station change)
        document.addEventListener('click', function(e) {
            if (e.target.closest('.station') || e.target.closest('#playbutton') || e.target.closest('#modestopped')) {
                clearTimeout(coverClickTimer);
                coverClickTimer = setTimeout(checkAndUpdateCover, 1200);
            }
        });

        // Observe metadata changes (element with id 'meta') and update cover once per title change
        const metaEl = document.getElementById('meta');
        if (metaEl) {
            const mo = new MutationObserver((mutations) => {
                mutations.forEach(m => {
                    if (m.type === 'characterData' || m.type === 'childList') {
                        clearTimeout(coverMutationTimer);
                        coverMutationTimer = setTimeout(checkAndUpdateCover, 500);
                    }
                });
            });
            mo.observe(metaEl, { characterData: true, childList: true, subtree: true });
        }

        checkAndUpdateCover();
        setTimeout(checkAndUpdateCover, 2000);
        isInitialized = true;
        console.log('[CoverArt] System Initialized');
    }

    // Старт
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initCoverSystem);
    } else {
        initCoverSystem();
    }
})();
