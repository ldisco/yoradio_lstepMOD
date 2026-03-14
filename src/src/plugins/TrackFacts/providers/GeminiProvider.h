// GeminiProvider.h — Провайдер фактов Google Gemini AI
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
#ifndef GEMINI_PROVIDER_H
#define GEMINI_PROVIDER_H

#include "FactProvider.h"

class GeminiProvider : public FactProvider {
public:
  FactResult fetchFact(const String& artist, const String& title) override;
  const char* name() const override { return "Gemini"; }
  bool needsApiKey() const override { return true; }
};

#endif
