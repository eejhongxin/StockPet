#include "shengsheng_avatar.h"

#include <string.h>

#include "shengsheng_avatar_data.inc"

const lisa_ui_anim_ext_config_t *shengsheng_avatar_get(const char *name)
{
    if (name == NULL) {
        return &shengsheng_idle;
    }
    if (strcmp(name, "wakeup") == 0 || strcmp(name, "listening") == 0) {
        return &shengsheng_listening;
    }
    if (strcmp(name, "wait") == 0 || strcmp(name, "thinking") == 0) {
        return &shengsheng_thinking;
    }
    if (strcmp(name, "speaking") == 0 || strcmp(name, "talking") == 0) {
        return &shengsheng_speaking;
    }
    if (strcmp(name, "sleepy") == 0 || strcmp(name, "sleep") == 0) {
        return &shengsheng_sleepy;
    }
    if (strcmp(name, "happy") == 0 || strcmp(name, "laugh") == 0 ||
        strcmp(name, "laughing") == 0 || strcmp(name, "excited") == 0 ||
        strcmp(name, "love") == 0 || strcmp(name, "loving") == 0 ||
        strcmp(name, "smile") == 0) {
        return &shengsheng_happy;
    }
    if (strcmp(name, "angry") == 0 || strcmp(name, "anger") == 0) {
        return &shengsheng_angry;
    }
    if (strcmp(name, "sad") == 0 || strcmp(name, "cry") == 0 ||
        strcmp(name, "crying") == 0) {
        return &shengsheng_sad;
    }
    return &shengsheng_idle;
}
