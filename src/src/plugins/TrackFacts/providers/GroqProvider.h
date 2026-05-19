// GroqProvider.h — факты через Groq OpenAI-compatible API (быстрый inference, бесплатный tier)
#ifndef GROQ_PROVIDER_H
#define GROQ_PROVIDER_H

#include "FactProvider.h"

class GroqProvider : public FactProvider {
public:
  FactResult fetchFact(const String& artist, const String& title,
                       const FactFetchContext* ctx) override;
  const char* name() const override { return "Groq"; }
  bool needsApiKey() const override { return true; }
};

#endif
