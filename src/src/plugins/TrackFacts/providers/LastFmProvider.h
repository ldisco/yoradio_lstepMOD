// LastFmProvider.h — Провайдер фактов Last.fm
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
#ifndef LASTFM_PROVIDER_H
#define LASTFM_PROVIDER_H

#include "FactProvider.h"

class LastFmProvider : public FactProvider {
public:
  FactResult fetchFact(const String& artist, const String& title) override;
  const char* name() const override { return "LastFM"; }
  bool needsApiKey() const override { return true; }
};

#endif
