#ifndef PSX_SAVESTATE_MENU_H
#define PSX_SAVESTATE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psx_savestate_menu_set_state(int open, int selected_slot);
/* System menu (ESC): shares this module's panel and compositing path. */
void psx_system_menu_set_state(int open, int selected);
void psx_system_menu_set_items(int open, int selected,
                               const char *const *labels, int count);
int  psx_system_menu_item_count(void);
void psx_savestate_menu_note_slots_changed(void);
/* 0 = none, 1 = confirming a save, 2 = confirming a load. */
void psx_savestate_menu_set_confirm(int mode);
int  psx_savestate_menu_needs_present(void);
int  psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_MENU_H */
