// iTunesProvider.h — Провайдер фактов iTunes Search API
// v0.4.0 — Новый провайдер: бесплатный, без ключа, быстрый
#ifndef ITUNES_PROVIDER_H
#define ITUNES_PROVIDER_H

#include "FactProvider.h"

class iTunesProvider : public FactProvider {
public:
  FactResult fetchFact(const String& artist, const String& title) override;
  const char* name() const override { return "iTunes"; }
  bool needsApiKey() const override { return false; }
};

#endif
