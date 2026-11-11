#ifndef CUI_VULKAN_H
#define CUI_VULKAN_H

/* A minimal CUI renderer on Vulkan: one pipeline, one shader, one texture
 * array. Pair it with an SDL2 window (SDL_WINDOW_VULKAN) and the CUI core
 * (cui_create / cui_begin_frame / cui_get_draw_data). */

#include <cui/cui.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cui_vk cui_vk;

cui_vk *cui_vk_create(SDL_Window *window);
void cui_vk_destroy(cui_vk *vk);

/* Renderer callbacks for cui_create / cui_set_renderer. */
cui_renderer_iface cui_vk_iface(cui_vk *vk);

/* Call after the window changed size; the swapchain is rebuilt on the next render. */
void cui_vk_resize(cui_vk *vk);

/* Acquires, records, submits and presents one frame. Returns false when the
 * swapchain was out of date and was rebuilt; draw the frame again. */
bool cui_vk_render(cui_vk *vk, const cui_draw_data *dd);

#ifdef __cplusplus
}
#endif

#endif
