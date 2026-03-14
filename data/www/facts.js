// TrackFacts - модуль для отображения фактов о композициях
// v0.4.1 - Зелёный цвет, две строки, замедленный скролл, защита от пустого факта
// Работает независимо, как и CoverArt плагин
// Автор: GitHub Copilot, 2026

(function() {
    'use strict';

    console.log('[TrackFacts] Module loaded v0.4.1');
    
    const CONFIG = {
        API_ENDPOINT: '/api/current-fact',
        FACT_SHOW_TIME: 15000,    // Показываем факт 15 секунд
        PAUSE_TIME: 15000,        // Пауза 15 секунд (показываем название)
        CHECK_INTERVAL: 4000,     // Проверяем каждые 4 секунды
        FETCH_DELAY: 2000         // Задержка перед началом цикла после смены трека
    };
    
    let lastTitle = '';
    let lastRawFacts = '';
    let currentFacts = [];
    let currentFactIndex = 0;
    let isShowingFact = false;
    let cycleTimer = null;
    let checkInterval = null;
    
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
        return false;
    }
    
    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    // Сброс всего состояния
    function resetAll() {
        if (cycleTimer) {
            clearTimeout(cycleTimer);
            cycleTimer = null;
        }
        isShowingFact = false;
        currentFacts = [];
        currentFactIndex = 0;
        lastRawFacts = '';
        const meta = getMetaElement();
        if (meta) {
            meta.classList.remove('track-fact');
            meta.style.fontSize = '';
            meta.style.color = '';
            meta.style.fontStyle = '';
            meta.style.lineHeight = '';
            if (isPlayerPlaying() && lastTitle && !isSystemMetaValue(meta.textContent)) {
                meta.textContent = lastTitle;
            }
        }
    }
    
    // Экспортируем функцию сброса глобально для вызова при смене режима
    window.resetTrackFacts = function() {
        lastTitle = '';
        resetAll();
        console.log('[TrackFacts] Reset by mode change');
    };

    // Отображение следующего элемента (Факт или Название)
    function displayNext() {
        const meta = getMetaElement();
        if (!meta || !isPlayerPlaying()) {
            resetAll();
            return;
        }

        if (!isShowingFact && currentFacts.length > 0) {
            // ПОКАЗЫВАЕМ ФАКТ
            const fact = currentFacts[currentFactIndex];
            
            // [v0.4.1] Защита от пустого факта — не показываем пустую бегущую строку
            if (!fact || fact.trim().length === 0) {
                currentFactIndex = (currentFactIndex + 1) % currentFacts.length;
                cycleTimer = setTimeout(displayNext, 1000);
                return;
            }
            
            meta.classList.add('track-fact');
            // [v0.4.3] Цвет из CSS темы, статичный текст без бегущей строки
            meta.style.fontSize = '1.2em';
            meta.style.color = '';
            meta.style.fontStyle = 'italic';
            meta.style.lineHeight = '1.4';

            // Простой текст без обёртки marquee — CSS -webkit-line-clamp ограничит до 2 строк
            meta.textContent = '💡 ' + fact;
            
            isShowingFact = true;
            currentFactIndex = (currentFactIndex + 1) % currentFacts.length;
            
            cycleTimer = setTimeout(displayNext, CONFIG.FACT_SHOW_TIME);
            console.log('[TrackFacts] Showing fact:', fact.substring(0, 30));
        } else {
            // ПОКАЗЫВАЕМ НАЗВАНИЕ (ПАУЗА)
            meta.classList.remove('track-fact');
            meta.style.fontSize = '';
            meta.style.color = '';
            meta.style.fontStyle = '';
            meta.style.lineHeight = '';
            meta.textContent = lastTitle;
            
            isShowingFact = false;
            cycleTimer = setTimeout(displayNext, CONFIG.PAUSE_TIME);
            console.log('[TrackFacts] Showing title (pause)');
        }
    }
    
    async function checkAndDisplayFact() {
        if (!isPlayerPlaying()) {
            resetAll();
            return;
        }
        
        try {
            const response = await fetch(CONFIG.API_ENDPOINT + '?t=' + Date.now());
            if (!response.ok) return;
            
            const data = await response.json();
            if (!data || !data.title) return;
            
            // Если трек сменился
            if (data.title !== lastTitle) {
                console.log('[TrackFacts] Track changed to:', data.title);
                lastTitle = data.title;
                resetAll(); // Сразу убираем старый факт и очищаем все массивы
                
                // [v0.4.1] Фильтруем пустые факты
                const facts = (data.facts || []).filter(function(f) { return f && f.trim().length > 0; });
                lastRawFacts = facts.join('###');
                
                if (facts.length > 0) {
                    currentFacts = facts;
                    currentFactIndex = 0;
                    cycleTimer = setTimeout(displayNext, CONFIG.FETCH_DELAY);
                }
            } else {
                // Тот же трек, проверяем не обновились ли факты
                const facts = (data.facts || []).filter(function(f) { return f && f.trim().length > 0; });
                const newRawFacts = facts.join('###');
                if (newRawFacts !== lastRawFacts && newRawFacts.length > 0) {
                    console.log('[TrackFacts] Facts updated for current track');
                    lastRawFacts = newRawFacts;
                    currentFacts = facts;
                    // Если цикл не запущен - запустим
                    if (!cycleTimer && !isShowingFact) {
                        displayNext();
                    }
                }
                // [FIX] Если сервер говорит, что фактов нет и нет ожидания запроса,
                // очищаем локальный кэш, чтобы диалог и строка совпадали.
                if (facts.length === 0 && !data.pending) {
                    resetAll();
                }
            }

            // Если фактов нет и запрос ожидается — показываем индикатор рядом с названием
            const meta = getMetaElement();
            if (meta && !isShowingFact && currentFacts.length === 0 && data.pending) {
                meta.textContent = lastTitle + ' ⏳';
            }
        } catch (error) {
            console.error('[TrackFacts] Error:', error);
        }
    }
    
    function init() {
        setInterval(checkAndDisplayFact, CONFIG.CHECK_INTERVAL);
        checkAndDisplayFact();
    }
    
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
