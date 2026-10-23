#ifndef _my_theme_h
#define _my_theme_h

/*
    Theming of color displays
    DSP_ILI9488 (DevKit S3) и прочие, для которых options.h тянет src/mytheme.h
    ***********************************************************************
    *    !!! This file must be in the root directory of the sketch !!!    *
    ***********************************************************************
    Тема как у JC2432 / ST7789 (mytheme_st7789_170.h, yoRadio sv_09434_2):
    чёрный фон, жёлтое имя станции без плашки, бирюзовые часы.
*/
#define ENABLE_THEME
#ifdef  ENABLE_THEME

/*-----------------------------------------------------------------------------------------------*/
/*       | COLORS             |   values (0-255)  |                                              */
/*       | color name         |    R    G    B    |                                              */
/*-----------------------------------------------------------------------------------------------*/
#define COLOR_BACKGROUND          0, 0, 0
#define COLOR_STATION_NAME        251, 219, 0
#define COLOR_STATION_BG          0, 0, 0
#define COLOR_STATION_FILL        0, 0, 0
#define COLOR_SNG_TITLE_1         255, 136, 0
#define COLOR_SNG_TITLE_2         185, 185, 185
#define COLOR_WEATHER             0, 255, 0
#define COLOR_WEATHER_CITY        255, 255, 255
#define COLOR_VU_MAX              210, 0, 0
#define COLOR_VU_MIN              0, 191, 255
#define COLOR_CLOCK               10, 245, 245
#define COLOR_CLOCK_BG            0, 0, 0
#define COLOR_SECONDS             10, 245, 245
#define COLOR_DAY_OF_W            240, 240, 240
#define COLOR_DATE                255, 255, 255
#define COLOR_HEAP                0, 0, 255
#define COLOR_BUFFER              255, 105, 180
#define COLOR_IP                  165, 162, 132
#define COLOR_VOLUME_VALUE        41, 189, 207
#define COLOR_RSSI                0, 70, 255
#define COLOR_VOLBAR_OUT          255, 255, 0
#define COLOR_VOLBAR_IN           255, 236, 41
#define COLOR_DIVIDER             255, 255, 0
#define COLOR_BITRATE             186, 187, 188

#define COLOR_PL_CURRENT          255, 255, 0
#define COLOR_PL_CURRENT_BG       30, 30, 30
#define COLOR_PL_CURRENT_FILL     60, 60, 60
/* Соседи выбранной строки: ещё тусклее (после 210→120 на железе — мало). */
#define COLOR_PLAYLIST_0          140, 140, 140
#define COLOR_PLAYLIST_1          110, 110, 110
#define COLOR_PLAYLIST_2           85,  85,  85
#define COLOR_PLAYLIST_3           65,  65,  65
#define COLOR_PLAYLIST_4           50,  50,  50

#endif  /* #ifdef  ENABLE_THEME */
#endif  /* #define _my_theme_h  */
