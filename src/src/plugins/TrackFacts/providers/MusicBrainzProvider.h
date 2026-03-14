// MusicBrainzProvider.h — Провайдер фактов MusicBrainz
// v0.4.0 — Бесплатный, без API ключа. Поддержка VPS Relay.
#ifndef MUSICBRAINZ_PROVIDER_H
#define MUSICBRAINZ_PROVIDER_H

#include "FactProvider.h"

class MusicBrainzProvider : public FactProvider {
public:
  FactResult fetchFact(const String& artist, const String& title) override;
  const char* name() const override { return "MusicBrainz"; }
  bool needsApiKey() const override { return false; }

private:
  // [v0.3.23] Запрос через VPS Relay (TCP -> SSL прокси)
  FactResult fetchViaProxy(const String& artist, const String& title);
  
  // Прямой запрос к musicbrainz.org (fallback, если прокси недоступен)
  FactResult fetchDirect(const String& artist, const String& title);
  
  // Формирование текста факта из данных JSON-ответа MusicBrainz
  String buildFactString(const String& date, const String& country, const String& tagsList);
};

#endif
