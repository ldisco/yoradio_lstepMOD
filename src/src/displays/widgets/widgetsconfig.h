#ifndef widgetsconfig_h
#define widgetsconfig_h

enum WidgetAlign { WA_LEFT, WA_CENTER, WA_RIGHT };
enum BitrateFormat { BF_UNKNOWN, BF_MP3, BF_AAC, BF_FLAC, BF_OGG, BF_WAV };

struct WidgetConfig {
  uint16_t left; 
  uint16_t top; 
  uint16_t textsize;
  WidgetAlign align;
};

struct ScrollConfig {
  WidgetConfig widget;
  uint16_t buffsize;
  bool uppercase;
  uint16_t width;
  uint16_t startscrolldelay;
  uint8_t scrolldelta;
  uint16_t scrolltime;
};

struct FillConfig {
  WidgetConfig widget;
  uint16_t width;
  uint16_t height;
  bool outlined;
};

struct ProgressConfig {
  uint16_t speed;
  uint16_t width;
  uint16_t barwidth;
};

struct VUBandsConfig {
  uint16_t width;
  uint16_t height;
  uint8_t  space;
  uint8_t  vspace;
  uint8_t  perheight;
  uint8_t  fadespeed;
};

/*
 * Геометрия отдельного FFT-виджета.
 * В отличие от VUBandsConfig здесь bandCount — число частотных диапазонов,
 * а segments — число вертикальных LED-ячеек каждой полосы.
 */
struct SpectrumConfig {
  uint16_t width;
  uint16_t height;
  uint8_t bandCount;
  uint8_t segments;
  uint8_t bandGap;
  uint8_t segmentGap;
  uint16_t frameIntervalMs;
};

struct MoveConfig {
  uint16_t x;
  uint16_t y;
  int16_t width;
};

struct BitrateConfig {
  WidgetConfig widget;
  uint16_t dimension;
};

#endif
