#ifndef utf8RusGFX_h
#define  utf8RusGFX_h

char* DspCore::utf8Rus(const char* str, bool uppercase) {
  int index = 0;
  static char strn[BUFLEN];
  bool E = false;
  strlcpy(strn, str, BUFLEN);
  if (uppercase) {
    bool next = false;
    for (char *iter = strn; *iter != '\0'; ++iter)
    {
      if (E) {
        E = false;
        continue;
      }
      uint8_t rus = (uint8_t) * iter;
      if (rus == 208 && (uint8_t) * (iter + 1) == 129) {
        *iter = (char)209;
        *(iter + 1) = (char)145;
        E = true;
        continue;
      }
      if (rus == 209 && (uint8_t) * (iter + 1) == 145) {
        *iter = (char)209;
        *(iter + 1) = (char)145;
        E = true;
        continue;
      }
      if (next) {
        if (rus >= 128 && rus <= 143) *iter = (char)(rus + 32);
        if (rus >= 176 && rus <= 191) *iter = (char)(rus - 32);
        next = false;
      }
      if (rus == 208) next = true;
      if (rus == 209) {
        *iter = (char)208;
        next = true;
      }
      *iter = toupper(*iter);
    }
  }
  if(L10N_LANGUAGE==EN) return strn;
  while (strn[index])
  {
    if ((uint8_t)strn[index] >= 0xBF)
    {
      int skip = 1;
      switch ((uint8_t)strn[index]) {
        case 0xD0: {
            if ((uint8_t)strn[index + 1] == 0x81) {
              strn[index] = 0xA8;
              break;
            }
            if ((uint8_t)strn[index + 1] >= 0x90 && (uint8_t)strn[index + 1] <= 0xBF) strn[index] = (uint8_t)strn[index + 1] + 0x30;
            break;
          }
        case 0xD1: {
            if ((uint8_t)strn[index + 1] == 0x91) {
              strn[index] = 0xB8;
              break;
            }
            if ((uint8_t)strn[index + 1] >= 0x80 && (uint8_t)strn[index + 1] <= 0x8F) strn[index] = (uint8_t)strn[index + 1] + 0x70;
            break;
          }
        case 0xE2: {
            if ((uint8_t)strn[index + 1] == 0x80 && ((uint8_t)strn[index + 2] == 0x93 || (uint8_t)strn[index + 2] == 0x94)) {
              strn[index] = '-';
              skip = 2;
            }
            break;
          }
      }
      int sind = index + 1 + skip;
      while (strn[sind]) {
        strn[sind - skip] = strn[sind];
        sind++;
      }
      strn[sind - skip] = 0;
    }
    index++;
  }
  return strn;
}

#endif
