#pragma once

// Which board this build is for. The build environment defines one MWC_BOARD_* macro; with none, it is the
// original Seengreat HUB75 controller. Everything board-specific keys off these names, never off "not the other one".
#if defined(MWC_BOARD_LCD4848) + defined(MWC_BOARD_C3TV) + defined(MWC_BOARD_HUB75) > 1
#error "define only one MWC_BOARD_* macro"
#endif
#if !defined(MWC_BOARD_LCD4848) && !defined(MWC_BOARD_C3TV) && !defined(MWC_BOARD_HUB75)
#define MWC_BOARD_HUB75
#endif

#if defined(MWC_BOARD_LCD4848)
#define MWC_BOARD_NAME "Guition ESP32-4848S040 4-inch LCD"
#define MWC_UPDATE_KEY "lcd4848"          // entry under "boards" in the update manifest
#elif defined(MWC_BOARD_C3TV)
#define MWC_BOARD_NAME "Spotpear ESP32-C3 1.44-inch mini TV"
#define MWC_UPDATE_KEY "c3tv"
#else
#define MWC_BOARD_NAME "Seengreat RGB Matrix HUB75 S3"
#define MWC_UPDATE_KEY ""                  // the HUB75 build uses the manifest's top-level entries
#endif
