/* Controlled PipeWire-only consumer for the CF-20 source.
 *
 * This diagnostic never opens V4L2, libcamera, ImgU or the camera app.  It
 * connects an input pw_stream to the named/serial PipeWire source and counts
 * received buffers for a bounded interval.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>

#define WIDTH 640u
#define HEIGHT 480u
#define FPS 15u
#define TEST_SECONDS 6u
#define BYTES_PER_PIXEL 3u
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

struct consumer_data {
	struct pw_main_loop *loop;
	struct pw_context *resolve_context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_hook registry_listener;
	struct spa_hook core_listener;
	struct spa_source *timer;
	const char *target;
	uint32_t target_id;
	bool registry_done;
	uint32_t frames;
	uint32_t bytes;
	uint32_t invalid_buffers;
	uint32_t nonzero_frames;
	bool saw_streaming;
};

static bool target_is_number(const char *target, uint64_t *value)
{
	char *end = NULL;
	unsigned long long parsed;

	if (target == NULL || target[0] == '\0')
		return false;
	errno = 0;
	parsed = strtoull(target, &end, 10);
	if (errno != 0 || end == target || *end != '\0')
		return false;
	*value = parsed;
	return true;
}

static void on_global(void *userdata, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct consumer_data *data = userdata;
	const char *name;
	const char *serial;
	uint64_t numeric_target;
	char id_text[32];

	(void)permissions;
	(void)version;
	if (data->target == NULL || strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ||
		props == NULL)
		return;

	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
	snprintf(id_text, sizeof(id_text), "%u", id);
	if ((name != NULL && strcmp(name, data->target) == 0) ||
		(serial != NULL && strcmp(serial, data->target) == 0) ||
		(strcmp(id_text, data->target) == 0) ||
		(target_is_number(data->target, &numeric_target) &&
			numeric_target == id)) {
		data->target_id = id;
		fprintf(stderr,
			"cf20-native-consumer: resolved target=%s node-id=%u\n",
			data->target, id);
	}
}

static void on_global_remove(void *userdata, uint32_t id)
{
	struct consumer_data *data = userdata;
	if (data->target_id == id)
		data->target_id = PW_ID_ANY;
}

static const struct pw_registry_events registry_events = {
	.version = PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

static void on_core_done(void *userdata, uint32_t id, int seq)
{
	struct consumer_data *data = userdata;
	(void)id;
	(void)seq;
	data->registry_done = true;
	pw_main_loop_quit(data->loop);
}

static const struct pw_core_events core_events = {
	.version = PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
};

static void on_process(void *userdata)
{
	struct consumer_data *data = userdata;
	struct pw_buffer *buffer = pw_stream_dequeue_buffer(data->stream);

	if (buffer == NULL)
		return;

	if (buffer->buffer == NULL || buffer->buffer->n_datas == 0) {
		data->invalid_buffers++;
	} else {
		struct spa_data *plane = &buffer->buffer->datas[0];
		struct spa_chunk *chunk = plane->chunk;
		bool nonzero = false;

		if (plane->data == NULL || plane->maxsize < FRAME_SIZE ||
			chunk == NULL || chunk->offset != 0 ||
			chunk->size != FRAME_SIZE ||
			chunk->stride != WIDTH * BYTES_PER_PIXEL) {
			data->invalid_buffers++;
		} else {
			const uint8_t *pixels = plane->data;

			/* Sampling is enough to distinguish a real RGB frame from the
			 * zeroed/poisoned buffers that caused the previous preview
			 * flicker, without making the diagnostic itself expensive. */
			for (size_t offset = 0; offset < FRAME_SIZE; offset += 64U) {
				if (pixels[offset] != 0) {
					nonzero = true;
					break;
				}
			}
			data->frames++;
			data->bytes += chunk->size;
			if (nonzero)
				data->nonzero_frames++;
		}
	}

	pw_stream_queue_buffer(data->stream, buffer);
}

static void on_state_changed(void *userdata, enum pw_stream_state old_state,
		enum pw_stream_state state, const char *error)
{
	struct consumer_data *data = userdata;
	(void)old_state;

	fprintf(stderr, "cf20-native-consumer: stream=%s%s%s\n",
		pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");

	if (state == PW_STREAM_STATE_STREAMING)
		data->saw_streaming = true;
	else if (state == PW_STREAM_STATE_PAUSED)
		pw_stream_set_active(data->stream, true);

	if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
		pw_main_loop_quit(data->loop);
}

static void on_param_changed(void *userdata, uint32_t id,
		const struct spa_pod *param)
{
	struct consumer_data *data = userdata;
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
		sizeof(pod_buffer));
	const struct spa_pod *params[3];
	uint32_t n_params = 0;
	int result;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(WIDTH * HEIGHT * 3),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(WIDTH * 3),
		SPA_PARAM_BUFFERS_dataType,
		SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_MemPtr));
	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
	result = pw_stream_update_params(data->stream, params, n_params);
	fprintf(stderr, "cf20-native-consumer: format selected, buffer params=%d\n",
		result);
	if (result < 0)
		pw_stream_set_error(data->stream, result, "buffer negotiation failed");
}

static const struct pw_stream_events stream_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void on_timeout(void *userdata, uint64_t expirations)
{
	struct consumer_data *data = userdata;
	(void)expirations;
	pw_main_loop_quit(data->loop);
}

static void on_signal(void *userdata, int signal_number)
{
	struct consumer_data *data = userdata;
	(void)signal_number;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char **argv)
{
	struct consumer_data data = { 0 };
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
		sizeof(pod_buffer));
	struct spa_video_info_raw video = SPA_VIDEO_INFO_RAW_INIT(
		.format = SPA_VIDEO_FORMAT_RGB,
		.size = SPA_RECTANGLE(WIDTH, HEIGHT),
		.framerate = SPA_FRACTION(FPS, 1));
	const struct spa_pod *params[1];
	const char *target = getenv("CF20_TARGET_OBJECT");
	struct pw_properties *properties;
	struct timespec timeout = { .tv_sec = TEST_SECONDS, .tv_nsec = 0 };
	int result = 1;

	/* A dash deliberately disables target.object for an autoconnect test. */
	if (target != NULL && strcmp(target, "-") == 0)
		target = NULL;
	else if (target == NULL || target[0] == '\0')
		target = "cf20-ov8858-raw";
	pw_init(&argc, &argv);
	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		goto cleanup;
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, on_signal, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, on_signal, &data);
	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
		on_timeout, &data);
	if (data.timer == NULL)
		goto cleanup_loop;

	properties = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Camera",
		PW_KEY_NODE_AUTOCONNECT, "true",
		NULL);
	if (target != NULL) {
		pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target);
		/* Keep the deprecated alias for older portal/policy paths. */
		pw_properties_set(properties, "node.target", target);
	}
	data.target = target;
	data.target_id = PW_ID_ANY;
	data.resolve_context = pw_context_new(
		pw_main_loop_get_loop(data.loop), NULL, 0);
	if (data.resolve_context == NULL)
		goto cleanup_loop;
	data.core = pw_context_connect(data.resolve_context, NULL, 0);
	/* The simple stream constructor owns the context internally.  Resolve the
	 * target through a separate short-lived core connection before connecting;
	 * this avoids relying on session-policy name matching, which can leave a
	 * stream in PAUSED with no link and therefore no frames. */
	if (data.core == NULL)
		goto cleanup_loop;
	if (target != NULL) {
		data.registry = pw_core_get_registry(data.core,
			PW_VERSION_REGISTRY, 0);
		if (data.registry == NULL ||
			pw_registry_add_listener(data.registry, &data.registry_listener,
				&registry_events, &data) < 0 ||
			pw_core_add_listener(data.core, &data.core_listener,
				&core_events, &data) < 0 ||
			pw_core_sync(data.core, PW_ID_CORE, 0) < 0)
			goto cleanup_loop;
		pw_main_loop_run(data.loop);
		if (!data.registry_done || data.target_id == PW_ID_ANY) {
			fprintf(stderr, "cf20-native-consumer: target not found: %s\n",
				target);
			goto cleanup_loop;
		}
	}
	data.stream = pw_stream_new_simple(pw_main_loop_get_loop(data.loop),
		"CF-20 native consumer", properties, &stream_events, &data);
	if (data.stream == NULL)
		goto cleanup_loop;
	if (target != NULL) {
		struct spa_dict_item target_items[] = {
			SPA_DICT_ITEM_INIT(PW_KEY_TARGET_OBJECT, target),
			SPA_DICT_ITEM_INIT("node.target", target),
		};
		struct spa_dict target_dict = SPA_DICT_INIT(target_items,
			SPA_N_ELEMENTS(target_items));
		pw_stream_update_properties(data.stream, &target_dict);
	}
	params[0] = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &video);
	if (pw_stream_connect(data.stream, PW_DIRECTION_INPUT,
			target != NULL ? data.target_id : PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_INACTIVE |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, 1) < 0) {
		fprintf(stderr, "cf20-native-consumer: stream connect failed: %s\n",
			strerror(errno));
		goto cleanup_stream;
	}

	pw_loop_update_timer(pw_main_loop_get_loop(data.loop), data.timer,
		&timeout, NULL, false);
	fprintf(stderr, "cf20-native-consumer: target=%s node-id=%u\n",
		target != NULL ? target : "<autoconnect>",
		target != NULL ? data.target_id : PW_ID_ANY);
	pw_main_loop_run(data.loop);

	printf("PIPEWIRE_NATIVE_CONSUMER_FRAMES=%u\n", data.frames);
	printf("PIPEWIRE_NATIVE_CONSUMER_BYTES=%u\n", data.bytes);
	printf("PIPEWIRE_NATIVE_CONSUMER_INVALID_BUFFERS=%u\n",
		data.invalid_buffers);
	printf("PIPEWIRE_NATIVE_CONSUMER_NONZERO_FRAMES=%u\n",
		data.nonzero_frames);
	printf("PIPEWIRE_NATIVE_CONSUMER_STREAMING=%s\n",
		data.saw_streaming ? "yes" : "no");
	result = data.frames > 0 && data.invalid_buffers == 0 &&
		data.nonzero_frames > 0 ? 0 : 1;

cleanup_stream:
	if (data.stream != NULL)
		pw_stream_destroy(data.stream);
cleanup_loop:
	if (data.core != NULL)
		pw_core_disconnect(data.core);
	if (data.resolve_context != NULL)
		pw_context_destroy(data.resolve_context);
	if (data.loop != NULL)
		pw_main_loop_destroy(data.loop);
cleanup:
	pw_deinit();
	return result;
}
