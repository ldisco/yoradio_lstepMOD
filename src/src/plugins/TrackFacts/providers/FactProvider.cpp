// FactProvider.cpp — общая логика промпта для AI-провайдеров
#include "FactProvider.h"

String FactProvider::makeMusicFactUserPrompt(const String& artist, const String& title,
                                             const FactFetchContext* ctx) const {
  const bool iterative =
      ctx && ctx->factTotal >= 2 && ctx->factIndex1 >= 1 && ctx->factIndex1 <= ctx->factTotal;

  const bool wantEnglish = (_language == ProviderLanguage::ENGLISH);

  String prompt;
  if (wantEnglish) {
    prompt =
        "In English, write a short fact of 19-22 words about the composition \"" + artist +
        " - " + title +
        "\". Use only claims you could support from widely available public information about this "
        "track or artist; do not invent instruments, genre, chart positions, or recording details. "
        "If reliable public information is sparse or you are unsure, reply with one short honest "
        "sentence stating that verifiable details are scarce (that sentence may be under 19 words). "
        "No lead-in phrases.";
  } else {
    prompt =
        "На русском языке напиши короткий факт из 19-22 слов про композицию \"" + artist +
        " - " + title +
        "\". Указывай только то, что можно опереть на общеизвестные открытые сведения об этом треке "
        "или авторе; не выдумывай инструменты, жанр, чарты или детали записи. Если достоверных "
        "публичных данных мало или ты не уверен — одно короткое честное предложение, что проверяемой "
        "информации об этой записи почти нет. Без вводных "
        "фраз.";
  }

  if (iterative) {
    if (wantEnglish) {
      prompt += " This is fact #" + String(ctx->factIndex1) + " of " + String(ctx->factTotal) +
                " for this track; pick another verifiable angle (release, credits, charts, cultural "
                "context, covers, documented trivia). Do not repeat or paraphrase prior facts; do not "
                "fabricate.";
    } else {
      prompt += " Это факт №" + String(ctx->factIndex1) + " из " + String(ctx->factTotal) +
                " про этот трек; выбери другой проверяемый ракурс (релиз, участники, чарты, "
                "культурный контекст, каверы, задокументированная деталь). Не повторяй и не "
                "перефразируй уже сказанное; не домысливай.";
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
