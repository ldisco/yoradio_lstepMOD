// TrackFactsProxy.h — compile-time / web URL прокси для Gemini/Groq/DeepSeek (VPS nginx).
#ifndef TRACKFACTS_PROXY_H
#define TRACKFACTS_PROXY_H

#include <Arduino.h>

#ifndef TRACKFACTS_PROXY_URL_LENGTH
#define TRACKFACTS_PROXY_URL_LENGTH 64
#endif

#if defined(TRACKFACTS_AI_PROXY) || defined(TRACKFACTS_GEMINI)
  #define TRACKFACTS_PROXY_UI_GEMINI 1
#else
  #define TRACKFACTS_PROXY_UI_GEMINI 0
#endif

#if defined(TRACKFACTS_AI_PROXY) || defined(TRACKFACTS_GROQ)
  #define TRACKFACTS_PROXY_UI_GROQ 1
#else
  #define TRACKFACTS_PROXY_UI_GROQ 0
#endif

#if defined(TRACKFACTS_AI_PROXY) || defined(TRACKFACTS_DEEPSEEK)
  #define TRACKFACTS_PROXY_UI_DEEPSEEK 1
#else
  #define TRACKFACTS_PROXY_UI_DEEPSEEK 0
#endif

inline String trackFactsCompileTimeProxyGemini() {
#ifdef TRACKFACTS_GEMINI
  {
    String s(TRACKFACTS_GEMINI);
    s.trim();
    if (s.length() > 0) return s;
  }
#endif
#ifdef TRACKFACTS_AI_PROXY
  {
    String s(TRACKFACTS_AI_PROXY);
    s.trim();
    if (s.length() > 0) return s;
  }
#endif
  return String();
}

inline String trackFactsCompileTimeProxyGroq() {
#ifdef TRACKFACTS_GROQ
  {
    String s(TRACKFACTS_GROQ);
    s.trim();
    if (s.length() > 0) return s;
  }
#endif
#ifdef TRACKFACTS_AI_PROXY
  {
    String s(TRACKFACTS_AI_PROXY);
    s.trim();
    if (s.length() > 0) return s;
  }
#endif
  return String();
}

inline String trackFactsCompileTimeProxyDeepSeek() {
#ifdef TRACKFACTS_DEEPSEEK
  {
    String s(TRACKFACTS_DEEPSEEK);
    s.trim();
    if (s.length() > 0) return s;
  }
#endif
#ifdef TRACKFACTS_AI_PROXY
  {
    String s(TRACKFACTS_AI_PROXY);
    s.trim();
    if (s.length() > 0) return s;
  }
#endif
  return String();
}

inline String trackFactsEffectiveProxyGemini(const char* webProxy) {
  if (webProxy != nullptr && webProxy[0] != '\0') {
    String s(webProxy);
    s.trim();
    if (s.length() > 0) return s;
  }
  return trackFactsCompileTimeProxyGemini();
}

inline String trackFactsEffectiveProxyGroq(const char* webProxy) {
  if (webProxy != nullptr && webProxy[0] != '\0') {
    String s(webProxy);
    s.trim();
    if (s.length() > 0) return s;
  }
  return trackFactsCompileTimeProxyGroq();
}

inline String trackFactsEffectiveProxyDeepSeek(const char* webProxy) {
  if (webProxy != nullptr && webProxy[0] != '\0') {
    String s(webProxy);
    s.trim();
    if (s.length() > 0) return s;
  }
  return trackFactsCompileTimeProxyDeepSeek();
}

#endif
