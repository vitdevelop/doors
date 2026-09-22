#include "copy_capture.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "once.h"
#include "server.h"
#include "toplevel.h"
#include "types.h"
#include "xwayland.h"
#include <drm_fourcc.h>
#include <math.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/addon.h>

// ext-image-copy-capture itself (sessions, frames, cursor sessions, buffer constraints, damage
// tracking) is wlroots' wlr_ext_image_copy_capture_manager_v1. What doors adds are the capture
// sources, which is what needs compositor knowledge:
//  - output sources hide windows that block themselves out of screen sharing
//  - the pointer of an output is offered as a cursor source, also for software cursors
// Toplevel sources are wlroots' scene node sources, created in foreign_capture.c.

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

#define MAX_BLOCKED_WINDOWS 128

struct blocked_node_state {
	struct wlr_scene_node *node;
	bool was_enabled;
};

static int disable_blocked_windows(struct blocked_node_state *states, int max_states) {
	int count = 0;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->node || !tl->node->client)
			continue;

		client_t *c = tl->node->client;
		if (!c->flags.block_out_from_screenshare)
			continue;
		if (!c->flags.shown && c->state != STATE_FULLSCREEN)
			continue;

		if (count >= max_states)
			break;
		wlr_log(WLR_DEBUG, "ext-copy-capture: disabling toplevel scene_tree=%p"
			" app_id=%s", (void *)&tl->scene_tree->node, c->app_id);

		states[count].node = &tl->scene_tree->node;
		states[count].was_enabled = tl->scene_tree->node.enabled;
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		count++;
	}

	xwayland_toplevel_t *xw;
	wl_list_for_each(xw, &server.xwayland.views, link) {
		if (!xw->node || !xw->node->client)
			continue;

		client_t *c = xw->node->client;
		if (!c->flags.block_out_from_screenshare)
			continue;
		if (!c->flags.shown && c->state != STATE_FULLSCREEN)
			continue;

		if (count >= max_states)
			break;
		wlr_log(WLR_DEBUG, "ext-copy-capture: disabling xwayland scene_tree=%p"
			" title=%s", (void *)&xw->scene_tree->node, c->title);

		states[count].node = &xw->scene_tree->node;
		states[count].was_enabled = xw->scene_tree->node.enabled;
		wlr_scene_node_set_enabled(&xw->scene_tree->node, false);
		count++;
	}

	return count;
}

static void restore_blocked_windows(struct blocked_node_state *states, int count) {
	for (int i = 0; i < count; i++)
		wlr_scene_node_set_enabled(states[i].node, states[i].was_enabled);
}

typedef struct output_source_t output_source_t;

typedef struct cursor_source_t {
	struct wlr_ext_image_capture_source_v1_cursor base;
	output_source_t *parent;

	// the cursor image as last rendered for clients, and what it was made from
	struct wlr_swapchain *swapchain;
	struct wlr_buffer *buffer;
	struct wlr_texture *texture;
	struct wlr_fbox src_box;
	uint32_t width, height;
	bool needs_frame;
} cursor_source_t;

struct output_source_t {
	struct wlr_ext_image_capture_source_v1 base;
	struct wlr_addon addon;
	struct wlr_output *output;
	struct wl_list link;

	// only used to render a copy of the output without the blocked out windows
	struct wlr_swapchain *swapchain;

	struct wl_listener output_commit;

	size_t num_started;
	bool cursors_locked;

	cursor_source_t cursor;
};

struct output_frame_event {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec when;
};

static struct wl_list output_sources;
static struct wl_listener cursor_frame_listener;
static struct wl_listener cursor_request_set_cursor_listener;
static struct wl_listener cursor_request_set_shape_listener;
static bool cursor_hooks_ready;

static const struct wlr_addon_interface output_source_addon_impl;
static const struct wlr_ext_image_capture_source_v1_interface output_source_impl;
static const struct wlr_ext_image_capture_source_v1_interface cursor_source_impl;

// Cursor source

static struct wlr_output_cursor *output_cursor_of(struct wlr_output *output) {
	struct wlr_output_cursor *cursor, *first = NULL;
	wl_list_for_each(cursor, &output->cursors, link) {
		if (cursor->enabled)
			return cursor;
		if (!first)
			first = cursor;
	}
	return first;
}

static bool cursor_source_render_image(cursor_source_t *cs, struct wlr_output_cursor *oc) {
	struct wlr_output *output = cs->parent->output;

	if (!cs->swapchain || cs->swapchain->width != (int)oc->width ||
			cs->swapchain->height != (int)oc->height) {
		const struct wlr_drm_format_set *formats = wlr_renderer_get_render_formats(output->renderer);
		const struct wlr_drm_format *format = formats ?
			wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888) : NULL;
		if (!format)
			return false;

		struct wlr_swapchain *swapchain = wlr_swapchain_create(server.allocator, oc->width, oc->height,
			format);
		if (!swapchain)
			return false;

		wlr_swapchain_destroy(cs->swapchain);
		cs->swapchain = swapchain;
		if (cs->buffer) {
			wlr_buffer_unlock(cs->buffer);
			cs->buffer = NULL;
		}

		wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&cs->base.base, swapchain,
			output->renderer);
		wl_signal_emit_mutable(&cs->base.base.events.constraints_update, NULL);
	}

	struct wlr_buffer *buffer = wlr_swapchain_acquire(cs->swapchain);
	if (!buffer)
		return false;

	struct wlr_fbox src_box = oc->src_box;
	if (src_box.width <= 0 || src_box.height <= 0)
		src_box = (struct wlr_fbox){0, 0, oc->texture->width, oc->texture->height};

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(output->renderer, buffer, NULL);
	if (!pass) {
		wlr_buffer_unlock(buffer);
		return false;
	}

	// transparent background first, the image may be smaller than the buffer
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = {.width = buffer->width, .height = buffer->height},
		.color = {0, 0, 0, 0},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = oc->texture,
		.src_box = src_box,
		.dst_box = {.width = buffer->width, .height = buffer->height},
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_unlock(buffer);
		return false;
	}

	if (cs->buffer)
		wlr_buffer_unlock(cs->buffer);
	cs->buffer = buffer;
	return true;
}

static void cursor_source_update(cursor_source_t *cs) {
	struct wlr_output_cursor *oc = output_cursor_of(cs->parent->output);
	bool visible = oc && oc->enabled && oc->visible && oc->texture && oc->width > 0 && oc->height > 0;

	if (!visible) {
		if (cs->base.entered) {
			cs->base.entered = false;
			wl_signal_emit_mutable(&cs->base.events.update, NULL);
		}
		return;
	}

	bool image_changed = !cs->buffer || oc->texture != cs->texture || oc->width != cs->width ||
		oc->height != cs->height || oc->src_box.x != cs->src_box.x || oc->src_box.y != cs->src_box.y;
	if (image_changed) {
		if (!cursor_source_render_image(cs, oc))
			return;
		cs->texture = oc->texture;
		cs->src_box = oc->src_box;
		cs->width = oc->width;
		cs->height = oc->height;
	}

	cs->base.entered = true;
	cs->base.x = lround(oc->x);
	cs->base.y = lround(oc->y);
	cs->base.hotspot.x = oc->hotspot_x;
	cs->base.hotspot.y = oc->hotspot_y;
	wl_signal_emit_mutable(&cs->base.events.update, NULL);

	if (image_changed || cs->needs_frame) {
		cs->needs_frame = false;

		pixman_region32_t damage;
		pixman_region32_init_rect(&damage, 0, 0, cs->buffer->width, cs->buffer->height);
		struct wlr_ext_image_capture_source_v1_frame_event event = {.damage = &damage};
		wl_signal_emit_mutable(&cs->base.base.events.frame, &event);
		pixman_region32_fini(&damage);
	}
}

static void cursor_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	cursor_source_t *cs = wl_container_of((struct wlr_ext_image_capture_source_v1_cursor *)base, cs,
		base);
	if (schedule_frame) {
		cs->needs_frame = true;
		cursor_source_update(cs);
	}
}

static void cursor_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *event) {
	(void)event;
	cursor_source_t *cs = wl_container_of((struct wlr_ext_image_capture_source_v1_cursor *)base, cs,
		base);
	if (!cs->buffer) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}

	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, cs->buffer,
			cs->parent->output->renderer)) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
	}
}

static const struct wlr_ext_image_capture_source_v1_interface cursor_source_impl = {
	.request_frame = cursor_source_request_frame,
	.copy_frame = cursor_source_copy_frame,
};

static void cursor_sources_update_all(void) {
	output_source_t *src;
	wl_list_for_each(src, &output_sources, link)
		cursor_source_update(&src->cursor);
}

static void handle_cursor_event(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	cursor_sources_update_all();
}

// Output source

static void output_source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	output_source_t *src = wl_container_of(base, src, base);
	src->num_started++;

	// software cursors are drawn into the output buffer, so keep them on while a session
	// wants the cursor painted (hardware cursors are not part of it)
	if (with_cursors && !src->cursors_locked) {
		wlr_output_lock_software_cursors(src->output, true);
		src->cursors_locked = true;
	}
}

static void output_source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	output_source_t *src = wl_container_of(base, src, base);
	if (src->num_started > 0 && --src->num_started == 0 && src->cursors_locked) {
		wlr_output_lock_software_cursors(src->output, false);
		src->cursors_locked = false;
	}
}

static void output_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	output_source_t *src = wl_container_of(base, src, base);
	if (schedule_frame)
		wlr_output_update_needs_frame(src->output);
}

// A copy of the output made without the blocked out windows, `states` are restored by the caller.
static struct wlr_buffer *output_source_render_without_blocked(output_source_t *src) {
	struct wlr_output *output = src->output;
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene, output);
	if (!scene_output)
		return NULL;

	if (!src->swapchain || src->swapchain->width != output->width ||
			src->swapchain->height != output->height) {
		const struct wlr_drm_format_set *formats = wlr_renderer_get_render_formats(output->renderer);
		const struct wlr_drm_format *format = formats ?
			wlr_drm_format_set_get(formats, output->render_format) : NULL;
		if (!format)
			return NULL;

		struct wlr_swapchain *swapchain = wlr_swapchain_create(server.allocator, output->width,
			output->height, format);
		if (!swapchain)
			return NULL;
		wlr_swapchain_destroy(src->swapchain);
		src->swapchain = swapchain;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_scene_output_state_options opts = {.swapchain = src->swapchain};
	if (!wlr_scene_output_build_state(scene_output, &state, &opts)) {
		wlr_output_state_finish(&state);
		return NULL;
	}

	struct wlr_buffer *buffer = state.buffer;
	wlr_buffer_lock(buffer);
	wlr_output_state_finish(&state);
	return buffer;
}

static void output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	output_source_t *src = wl_container_of(base, src, base);
	struct output_frame_event *event = wl_container_of(base_event, event, base);
	struct wlr_output *output = src->output;

	struct blocked_node_state blocked_states[MAX_BLOCKED_WINDOWS];
	int nblocked = disable_blocked_windows(blocked_states, MAX_BLOCKED_WINDOWS);

	struct wlr_buffer *buffer = event->buffer;
	if (nblocked > 0) {
		buffer = output_source_render_without_blocked(src);
		if (!buffer) {
			restore_blocked_windows(blocked_states, nblocked);
			wlr_ext_image_copy_capture_frame_v1_fail(frame,
				EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
			return;
		}
	}

	bool ok = wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buffer, output->renderer);

	if (buffer != event->buffer)
		wlr_buffer_unlock(buffer);
	restore_blocked_windows(blocked_states, nblocked);

	if (ok)
		wlr_ext_image_copy_capture_frame_v1_ready(frame, output->transform, &event->when);
}

static struct wlr_ext_image_capture_source_v1_cursor *output_source_get_pointer_cursor(
		struct wlr_ext_image_capture_source_v1 *base, struct wlr_seat *seat) {
	(void)seat;
	output_source_t *src = wl_container_of(base, src, base);

	// a new cursor session reads the state right away, so bring it up to date
	cursor_source_update(&src->cursor);
	return &src->cursor.base;
}

static const struct wlr_ext_image_capture_source_v1_interface output_source_impl = {
	.start = output_source_start,
	.stop = output_source_stop,
	.request_frame = output_source_request_frame,
	.copy_frame = output_source_copy_frame,
	.get_pointer_cursor = output_source_get_pointer_cursor,
};

static void output_source_update_constraints(output_source_t *src) {
	struct wlr_output *output = src->output;
	if (!output->enabled)
		return;

	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain))
		return;

	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&src->base, output->swapchain,
		output->renderer);
}

static void output_source_handle_output_commit(struct wl_listener *listener, void *data) {
	output_source_t *src = wl_container_of(listener, src, output_commit);
	struct wlr_output_event_commit *event = data;

	if (event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_RENDER_FORMAT |
			WLR_OUTPUT_STATE_ENABLED))
		output_source_update_constraints(src);

	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER))
		return;

	struct wlr_buffer *buffer = event->state->buffer;
	pixman_region32_t full_damage;
	pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

	struct output_frame_event frame_event = {
		.base = {
			.damage = (event->state->committed & WLR_OUTPUT_STATE_DAMAGE) ?
				&event->state->damage : &full_damage,
		},
		.buffer = buffer,
		.when = event->when,
	};
	wl_signal_emit_mutable(&src->base.events.frame, &frame_event);
	pixman_region32_fini(&full_damage);
}

static void output_source_addon_destroy(struct wlr_addon *addon) {
	output_source_t *src = wl_container_of(addon, src, addon);

	wlr_ext_image_capture_source_v1_cursor_finish(&src->cursor.base);
	wlr_ext_image_capture_source_v1_finish(&src->base);

	wl_list_remove(&src->output_commit.link);
	wl_list_remove(&src->link);
	wlr_addon_finish(&src->addon);

	if (src->cursors_locked)
		wlr_output_lock_software_cursors(src->output, false);
	if (src->cursor.buffer)
		wlr_buffer_unlock(src->cursor.buffer);
	wlr_swapchain_destroy(src->cursor.swapchain);
	wlr_swapchain_destroy(src->swapchain);
	free(src);
}

static const struct wlr_addon_interface output_source_addon_impl = {
	.name = "doors_output_image_capture_source",
	.destroy = output_source_addon_destroy,
};

static output_source_t *output_source_for(struct wlr_output *output) {
	struct wlr_addon *addon = wlr_addon_find(&output->addons, NULL, &output_source_addon_impl);
	if (addon) {
		output_source_t *src = wl_container_of(addon, src, addon);
		return src;
	}

	output_source_t *src = calloc(1, sizeof(*src));
	if (!src)
		return NULL;

	wlr_ext_image_capture_source_v1_init(&src->base, &output_source_impl);
	wlr_ext_image_capture_source_v1_cursor_init(&src->cursor.base, &cursor_source_impl);
	src->cursor.parent = src;
	src->output = output;
	wlr_addon_init(&src->addon, &output->addons, NULL, &output_source_addon_impl);
	wl_list_insert(&output_sources, &src->link);

	src->output_commit.notify = output_source_handle_output_commit;
	wl_signal_add(&output->events.commit, &src->output_commit);

	output_source_update_constraints(src);
	return src;
}

typedef struct output_capture_mgr_t {
	struct wl_global *global;
	struct wl_listener display_destroy;
} output_capture_mgr_t;

static void output_mgr_handle_create_source(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id, struct wl_resource *output_resource) {
	struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
	if (!wlr_output) {
		wlr_ext_image_capture_source_v1_create_resource(NULL, wl_client, id);
		return;
	}

	output_source_t *src = output_source_for(wlr_output);
	if (!src) {
		wl_resource_post_no_memory(mgr_resource);
		return;
	}

	wlr_ext_image_capture_source_v1_create_resource(&src->base, wl_client, id);
}

static void output_mgr_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *mgr_resource) {
	(void)wl_client;
	wl_resource_destroy(mgr_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_mgr_impl = {
	.create_source = output_mgr_handle_create_source,
	.destroy = output_mgr_handle_destroy,
};

static void output_mgr_bind(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id) {
	output_capture_mgr_t *mgr = data;
	struct wl_resource *resource = wl_resource_create(wl_client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &output_mgr_impl, mgr, NULL);
}

static void output_mgr_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	output_capture_mgr_t *mgr = wl_container_of(listener, mgr, display_destroy);
	wl_list_remove(&mgr->display_destroy.link);
	wl_global_destroy(mgr->global);
	free(mgr);
}

static output_capture_mgr_t *output_mgr;
static struct wlr_ext_image_copy_capture_manager_v1 *copy_mgr;

void image_copy_capture_init(void) {
	ONCE();
	wl_list_init(&output_sources);

	output_mgr = calloc(1, sizeof(*output_mgr));
	if (!output_mgr)
		goto err_out;

	output_mgr->global = wl_global_create(server.wl_display,
		&ext_output_image_capture_source_manager_v1_interface, 1, output_mgr, output_mgr_bind);
	if (!output_mgr->global)
		goto err_out;

	output_mgr->display_destroy.notify = output_mgr_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &output_mgr->display_destroy);

	copy_mgr = wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);
	if (!copy_mgr)
		goto err_out;

	// the cursor of an output moves and changes shape independently of its frames
	cursor_frame_listener.notify = handle_cursor_event;
	wl_signal_add(&server.cursor->events.frame, &cursor_frame_listener);
	if (server.seat) {
		cursor_request_set_cursor_listener.notify = handle_cursor_event;
		wl_signal_add(&server.seat->events.request_set_cursor, &cursor_request_set_cursor_listener);
	}
	if (server.cursor_shape_manager) {
		cursor_request_set_shape_listener.notify = handle_cursor_event;
		wl_signal_add(&server.cursor_shape_manager->events.request_set_shape,
			&cursor_request_set_shape_listener);
	}
	cursor_hooks_ready = true;

	wlr_log(WLR_INFO, "ext-image-copy-capture initialized");
	return;

err_out:
	wlr_log(WLR_ERROR, "Failed to initialize ext-image-copy-capture");
	image_copy_capture_fini();
}

void image_copy_capture_fini(void) {
	ONCE();
	if (cursor_hooks_ready) {
		wl_list_remove(&cursor_frame_listener.link);
		if (server.seat)
			wl_list_remove(&cursor_request_set_cursor_listener.link);
		if (server.cursor_shape_manager)
			wl_list_remove(&cursor_request_set_shape_listener.link);
		cursor_hooks_ready = false;
	}

	if (output_mgr) {
		wl_list_remove(&output_mgr->display_destroy.link);
		if (output_mgr->global)
			wl_global_destroy(output_mgr->global);
		free(output_mgr);
		output_mgr = NULL;
	}
	// copy_mgr is destroyed with the display
	copy_mgr = NULL;
}

struct wl_global *image_copy_capture_get_global(void) {
	return copy_mgr ? copy_mgr->global : NULL;
}

struct wl_global *image_capture_source_get_global(void) {
	return output_mgr ? output_mgr->global : NULL;
}
