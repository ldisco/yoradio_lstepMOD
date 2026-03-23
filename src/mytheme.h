#ifndef _my_theme_h
#define _my_theme_h

/*
    Theming of color displays
    DSP_ST7735, DSP_ST7789, DSP_ILI9341, DSP_GC9106, DSP_ILI9225, DSP_ST7789_240
    ***********************************************************************
    *    !!! This file must be in the root directory of the sketch !!!    *
    ***********************************************************************
    Uncomment (remove double slash //) from desired line to apply color
*/
#define ENABLE_THEME
#ifdef  ENABLE_THEME

/*-----------------------------------------------------------------------------------------------*/
/*       | COLORS             |   values (0-255)  |                                              */
/*       | color name         |    R    G    B    |                                              */
/*-----------------------------------------------------------------------------------------------*/
#define COLOR_BACKGROUND        0, 0, 0        /* background */
#define COLOR_STATION_NAME       2, 84, 0       /* station name */
#define COLOR_STATION_BG         231, 211, 90   /* station name background */
#define COLOR_STATION_FILL       231, 211, 90   /* station name fill */
#define COLOR_SNG_TITLE_1        255, 126, 38   /* first title */
#define COLOR_SNG_TITLE_2        227, 227, 227  /* second title */
#define COLOR_WEATHER            65, 255, 58    /* weather string */
#define COLOR_VU_MAX             255, 0, 0      /* VU meter max */
#define COLOR_VU_MIN             0, 255, 0      /* VU meter min */
#define COLOR_CLOCK              131, 99, 157   /* clock color */
#define COLOR_CLOCK_BG           0, 0, 0        /* clock background */
#define COLOR_SECONDS            41, 255, 55    /* seconds */
#define COLOR_DAY_OF_W           255, 255, 255  /* day of week */
#define COLOR_DATE               255, 255, 255  /* date */
#define COLOR_HEAP               255, 168, 162  /* heap */
#define COLOR_BUFFER             41, 255, 55    /* buffer */
//#define COLOR_IP                 231, 211, 90   /* IP address */
#define COLOR_IP                 131, 99, 157  /* IP address */
#define COLOR_VOLUME_VALUE       41, 189, 207   /* volume value */
#define COLOR_RSSI               0, 70, 255     /* RSSI */
#define COLOR_VOLBAR_OUT         255, 255, 0    /* volume bar outline */
#define COLOR_VOLBAR_IN          255, 236, 41   /* volume bar fill */
#define COLOR_DIVIDER            255, 255, 0    /* divider */
//#define COLOR_BITRATE            231, 211, 90   /* bitrate */
#define COLOR_BITRATE            186, 187, 188 

#define COLOR_PL_CURRENT         255, 255, 0    /* playlist current item */
#define COLOR_PL_CURRENT_BG      30, 30 , 30    /* playlist current item background */
#define COLOR_PL_CURRENT_FILL    60, 60, 60     /* playlist current item fill */
#define COLOR_PLAYLIST_0         255, 255, 255  /* playlist string 0 */
#define COLOR_PLAYLIST_1         170, 170, 170  /* playlist string 1 */
#define COLOR_PLAYLIST_2         140, 140, 140  /* playlist string 2 */
#define COLOR_PLAYLIST_3         90, 90, 90     /* playlist string 3 */
#define COLOR_PLAYLIST_4         60, 60, 60     /* playlist string 4 */


#endif  /* #ifdef  ENABLE_THEME */
#endif  /* #define _my_theme_h  */
