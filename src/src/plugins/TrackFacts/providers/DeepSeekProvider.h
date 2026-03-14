// DeepSeekProvider.h — Провайдер фактов DeepSeek AI
// v0.4.0 — Рефакторинг: вынесено из TrackFacts.cpp
#ifndef DEEPSEEK_PROVIDER_H
#define DEEPSEEK_PROVIDER_H

#include "FactProvider.h"

class DeepSeekProvider : public FactProvider {
public:
  FactResult fetchFact(const String& artist, const String& title) override;
  const char* name() const override { return "DeepSeek"; }
  bool needsApiKey() const override { return true; }
};

#endif
