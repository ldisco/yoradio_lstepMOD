// FactProvider.cpp — общая логика промпта для AI-провайдеров
#include "FactProvider.h"

String FactProvider::makeMusicFactUserPrompt(const String& artist, const String& title,
                                             const FactFetchContext* ctx) const {
  const bool iterative =
      ctx && ctx->factTotal >= 2 && ctx->factIndex1 >= 1 && ctx->factIndex1 <= ctx->factTotal;

  const bool wantEnglish = (_language == ProviderLanguage::ENGLISH);

  String prompt;
  if (wantEnglish) {
  // Интересно, но строго про ЭТОТ трек — без чужого жанра и без отказа «данных нет».
    prompt =
        "In English, one interesting fact of 19-22 words about \"" + artist + " - " + title +
        "\". The fact must match this exact artist and song; do not invent genre, club scene, "
        "dance floors, remix culture, or charts unless clearly implied by the artist or title. "
        "Use only widely known public facts you are confident about for this recording; do not "
        "guess. If details are thin, say something cautious tied only to this artist or title "
        "(no generic clichés). Never write that information is missing. No lead-in phrases.";
  } else {
  // То же: интересный факт только про эту запись, без подмены жанра (рок ≠ клуб/танцпол).
    prompt =
        "На русском языке один интересный факт из 19-22 слов про \"" + artist + " - " + title +
        "\". Факт только про этого исполнителя и эту песню; не приписывай другой жанр, клуб, "
        "танцпол, ремикс-культуру или чарты, если из имени и названия это не следует. Пиши только "
        "то, в чём уверен по общеизвестным сведениям об этой записи; не додумывай. Если деталей "
        "мало — осторожный факт только про этого исполнителя или смысл названия, без общих клише. "
        "Не пиши, что информации нет. Без вводных фраз.";
  }

  if (iterative) {
    if (wantEnglish) {
      prompt += " This is fact #" + String(ctx->factIndex1) + " of " + String(ctx->factTotal) +
                " for this track; another angle (release year, credits, lyrics theme, covers) only "
                "if it fits this artist and title. Do not repeat prior facts; do not invent.";
    } else {
      prompt += " Это факт №" + String(ctx->factIndex1) + " из " + String(ctx->factTotal) +
                " про этот трек; другой ракурс (год, участники, смысл названия, кавер) только если "
                "подходит этой записи. Не повторяй уже сказанное; не выдумывай.";
    }
    if (ctx->priorFactsBrief.length() > 0) {
      if (wantEnglish) {
        prompt += " Already given (do not repeat): ";
      } else {
        prompt += " Уже сказано (не дублируй): ";
      }
      prompt += ctx->priorFactsBrief;
    }
  }

  return prompt;
}
