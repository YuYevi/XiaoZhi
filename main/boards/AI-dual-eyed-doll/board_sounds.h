#ifndef BOARD_SOUNDS_H
#define BOARD_SOUNDS_H

#include <string_view>

namespace BoardSounds {

extern const char short_laugh_ogg_start[] asm("_binary_Short_laugh_ogg_start");
extern const char short_laugh_ogg_end[] asm("_binary_Short_laugh_ogg_end");
static const std::string_view OGG_SHORT_LAUGH{
    static_cast<const char*>(short_laugh_ogg_start),
    static_cast<size_t>(short_laugh_ogg_end - short_laugh_ogg_start)};

extern const char tsundere_ogg_start[] asm("_binary_tsundere_ogg_start");
extern const char tsundere_ogg_end[] asm("_binary_tsundere_ogg_end");
static const std::string_view OGG_TSUNDERE{
    static_cast<const char*>(tsundere_ogg_start),
    static_cast<size_t>(tsundere_ogg_end - tsundere_ogg_start)};

}  // namespace BoardSounds

#endif  // BOARD_SOUNDS_H
