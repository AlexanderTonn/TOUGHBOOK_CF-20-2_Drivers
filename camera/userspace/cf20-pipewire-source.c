/* CF-20 RGB source for PipeWire.
 *
 * The process receives complete 640x480 RGB frames on stdin and publishes a
 * real PipeWire video source.  It intentionally does not use GStreamer's
 * pipewiresink: that element creates a client stream which is visible in the
 * graph but cannot be resolved by a second pipewiresrc on this system.
 */

#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>

#define WIDTH 640u
#define HEIGHT 480u
#define FPS 15u
#define BYTES_PER_PIXEL 3u
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

struct source_data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *timer;

	pthread_t reader;
	pthread_mutex_t frame_lock;
	uint8_t *frame;
	uint8_t *incoming;
	bool have_frame;
	bool format_ready;
	bool stream_error_reported;
	atomic_bool stop_reader;
	uint32_t sequence;
};

static void fail_stream(struct source_data *data, const char *message)
{
	if (data->stream_error_reported)
		return;

	data->stream_error_reported = true;
	fprintf(stderr, "cf20-pipewire-source: %s\n", message);
	pw_stream_set_error(data->stream, -EINVAL, "%s", message);
}

static bool read_full(int fd, uint8_t *dst, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t count = read(fd, dst + done, size - done);
		if (count == 0)
			return false;
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		done += (size_t)count;
	}
	return true;
}

static bool read_first_frame(int fd, uint8_t *dst, size_t size)
{
	struct pollfd descriptor = { .fd = fd, .events = POLLIN };
	const int timeout_ms = 15000;
	int result;

	do {
		result = poll(&descriptor, 1, timeout_ms);
	} while (result < 0 && errno == EINTR);

	if (result <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
		return false;
	return read_full(fd, dst, size);
}

static void *reader_main(void *userdata)
{
	struct source_data *data = userdata;

	while (!atomic_load_explicit(&data->stop_reader, memory_order_relaxed)) {
		/* Read into private storage first.  The PipeWire process callback can
		 * then never observe a half-written frame. */
		if (!read_full(STDIN_FILENO, data->incoming, FRAME_SIZE))
			break;

		pthread_mutex_lock(&data->frame_lock);
		memcpy(data->frame, data->incoming, FRAME_SIZE);
		data->have_frame = true;
		pthread_mutex_unlock(&data->frame_lock);
	}

	return NULL;
}

static void on_process(void *userdata)
{
	struct source_data *data = userdata;
	struct pw_buffer *buffer;
	struct spa_buffer *spa_buffer;
	bool copied = false;

	buffer = pw_stream_dequeue_buffer(data->stream);
	if (buffer == NULL)
		return;

	spa_buffer = buffer->buffer;
	if (!data->format_ready) {
		pw_stream_queue_buffer(data->stream, buffer);
		fail_stream(data, "process callback before a valid format was selected");
		return;
	}

	if (spa_buffer == NULL || spa_buffer->n_datas == 0 ||
		spa_buffer->datas[0].data == NULL ||
		spa_buffer->datas[0].maxsize < FRAME_SIZE ||
		spa_buffer->datas[0].chunk == NULL) {
		char diagnostic[160];

		snprintf(diagnostic, sizeof(diagnostic),
			"invalid PipeWire buffer (datas=%u data=%p maxsize=%u)",
			spa_buffer ? spa_buffer->n_datas : 0u,
			spa_buffer && spa_buffer->n_datas > 0 ?
				spa_buffer->datas[0].data : NULL,
			spa_buffer && spa_buffer->n_datas > 0 ?
				spa_buffer->datas[0].maxsize : 0u);
		pw_stream_queue_buffer(data->stream, buffer);
		fail_stream(data, diagnostic);
		return;
	}

	{
		struct spa_chunk *chunk = spa_buffer->datas[0].chunk;

		pthread_mutex_lock(&data->frame_lock);
		if (data->have_frame) {
			/* Keep the lock across the copy: the reader replaces the shared
			 * frame in the same critical section. */
			memcpy(spa_buffer->datas[0].data, data->frame, FRAME_SIZE);
			copied = true;
		}
		pthread_mutex_unlock(&data->frame_lock);

		if (!copied)
			memset(spa_buffer->datas[0].data, 0, FRAME_SIZE);

		chunk->offset = 0;
		chunk->size = FRAME_SIZE;
		chunk->stride = WIDTH * BYTES_PER_PIXEL;
	}

	if (spa_buffer->n_metas > 0) {
		struct spa_meta_region *crop = spa_buffer_find_meta_data(
			spa_buffer, SPA_META_VideoCrop, sizeof(*crop));
		if (crop != NULL)
			crop->region = SPA_REGION(0, 0, WIDTH, HEIGHT);
	}

	if (spa_buffer->n_datas > 0) {
		struct spa_meta_header *header = spa_buffer_find_meta_data(
			spa_buffer, SPA_META_Header, sizeof(*header));
		if (header != NULL) {
			header->pts = (int64_t)pw_stream_get_nsec(data->stream);
			header->dts_offset = 0;
			header->flags = 0;
			header->seq = data->sequence++;
		}
	}

	pw_stream_queue_buffer(data->stream, buffer);
}

static void on_timeout(void *userdata, uint64_t expirations)
{
	struct source_data *data = userdata;
	(void)expirations;
	if (data->stream != NULL && pw_stream_is_driving(data->stream))
		pw_stream_trigger_process(data->stream);
}

static void on_state_changed(void *userdata, enum pw_stream_state old_state,
		enum pw_stream_state state, const char *error)
{
	struct source_data *data = userdata;
	(void)old_state;

	fprintf(stderr, "cf20-pipewire-source: stream=%s%s%s\n",
		pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");

	if (state == PW_STREAM_STATE_ERROR) {
		pw_main_loop_quit(data->loop);
		return;
	}

	if (state == PW_STREAM_STATE_STREAMING) {
		struct timespec timeout = { .tv_sec = 0, .tv_nsec = 1 };
		struct timespec interval = { .tv_sec = 0,
			.tv_nsec = (long)(1000000000u / FPS) };
		fprintf(stderr, "cf20-pipewire-source: driver=%s\n",
			pw_stream_is_driving(data->stream) ? "yes" : "no");
		if (pw_stream_is_driving(data->stream))
			pw_stream_trigger_process(data->stream);
		pw_loop_update_timer(pw_main_loop_get_loop(data->loop), data->timer,
			&timeout, &interval, false);
	} else if (state == PW_STREAM_STATE_PAUSED ||
		state == PW_STREAM_STATE_CONNECTING ||
		state == PW_STREAM_STATE_UNCONNECTED) {
		/* A consumer can disappear while the source process remains healthy.
		 * Keep the node alive and let PipeWire reconnect it instead of tearing
		 * down the capture chain and publishing a new node. */
		if (state == PW_STREAM_STATE_UNCONNECTED)
			data->format_ready = false;
		pw_loop_update_timer(pw_main_loop_get_loop(data->loop), data->timer,
			NULL, NULL, false);
	}
}

static void on_io_changed(void *userdata, uint32_t id, void *area, uint32_t size)
{
	(void)userdata;
	(void)id;
	(void)area;
	(void)size;
}

static void on_param_changed(void *userdata, uint32_t id,
		const struct spa_pod *param)
{
	struct source_data *data = userdata;
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
		sizeof(pod_buffer));
	const struct spa_pod *params[4];
	uint32_t n_params = 0;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	{
		struct spa_video_info_raw selected = { 0 };
		int parse_result = spa_format_video_raw_parse(param, &selected);

		if (parse_result < 0) {
			char diagnostic[160];

			snprintf(diagnostic, sizeof(diagnostic),
				"cannot parse selected video format (error=%d)",
				parse_result);
			fail_stream(data, diagnostic);
			return;
		}

		fprintf(stderr,
			"cf20-pipewire-source: selected format=%u size=%ux%u fps=%u/%u\n",
			(unsigned)selected.format, selected.size.width,
			selected.size.height, selected.framerate.num,
			selected.framerate.denom);

		if (selected.format != SPA_VIDEO_FORMAT_RGB ||
			selected.size.width != WIDTH ||
			selected.size.height != HEIGHT ||
			selected.framerate.num != FPS ||
			selected.framerate.denom != 1) {
			fail_stream(data,
				"consumer selected an unsupported video format; refusing corrupt frames");
			return;
		}

		data->format_ready = true;
	}

	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
		SPA_POD_CHOICE_RANGE_Int(8, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(FRAME_SIZE),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(WIDTH * BYTES_PER_PIXEL),
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

	int result = pw_stream_update_params(data->stream, params, n_params);
	fprintf(stderr, "cf20-pipewire-source: buffer negotiation result=%d\n",
		result);
	if (result < 0) {
		data->format_ready = false;
		pw_stream_set_error(data->stream, result,
			"buffer negotiation failed");
	}
}

static const struct pw_stream_events stream_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.io_changed = on_io_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void on_signal(void *userdata, int signal_number)
{
	struct source_data *data = userdata;
	(void)signal_number;
	atomic_store_explicit(&data->stop_reader, true, memory_order_relaxed);
	pw_main_loop_quit(data->loop);
}

int main(int argc, char **argv)
{
	struct source_data data = { 0 };
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
			sizeof(pod_buffer));
	struct spa_video_info_raw video = SPA_VIDEO_INFO_RAW_INIT(
		.format = SPA_VIDEO_FORMAT_RGB,
		.size = SPA_RECTANGLE(WIDTH, HEIGHT),
		.framerate = SPA_FRACTION(FPS, 1));
	const struct spa_pod *params[1];
	int result = 1;
	bool mutex_ready = false;

	(void)argv;
	pw_init(&argc, &argv);

	data.frame = malloc(FRAME_SIZE);
	data.incoming = malloc(FRAME_SIZE);
	if (data.frame == NULL || data.incoming == NULL)
		goto cleanup;
	atomic_init(&data.stop_reader, false);
	if (pthread_mutex_init(&data.frame_lock, NULL) != 0)
		goto cleanup;
	mutex_ready = true;
	if (!read_first_frame(STDIN_FILENO, data.frame, FRAME_SIZE)) {
		fprintf(stderr,
			"cf20-pipewire-source: no complete RGB frame received within 15 seconds\n");
		goto cleanup_mutex;
	}
	data.have_frame = true;

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		goto cleanup_mutex;
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, on_signal, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, on_signal, &data);
	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
			on_timeout, &data);
	if (data.timer == NULL)
		goto cleanup_loop;

	data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
	if (data.context == NULL)
		goto cleanup_loop;
	data.core = pw_context_connect(data.context, NULL, 0);
	if (data.core == NULL) {
		fprintf(stderr, "cf20-pipewire-source: PipeWire connect failed: %s\n",
			strerror(errno));
		goto cleanup_context;
	}

	data.stream = pw_stream_new(data.core, "CF-20 OV8858 raw camera",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CLASS, "Video/Source",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Camera",
			PW_KEY_NODE_NAME, "cf20-ov8858-raw",
			PW_KEY_NODE_DESCRIPTION, "Panasonic CF-20 rear camera (RAW CIO2)",
			PW_KEY_OBJECT_REGISTER, "true",
			PW_KEY_OBJECT_EXPORT, "true",
			PW_KEY_NODE_AUTOCONNECT, "true",
			PW_KEY_NODE_ASYNC, "true",
			PW_KEY_NODE_WANT_DRIVER, "true",
			PW_KEY_NODE_DRIVER, "true",
			PW_KEY_NODE_ALWAYS_PROCESS, "true",
			PW_KEY_NODE_RATE, "1/15",
			PW_KEY_STREAM_IS_LIVE, "true",
			PW_KEY_MEDIA_NAME, "CF-20 OV8858 raw camera",
			NULL));
	if (data.stream == NULL)
		goto cleanup_core;
	pw_stream_add_listener(data.stream, &data.stream_listener, &stream_events, &data);

	params[0] = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &video);
	uint32_t stream_flags = PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_ASYNC |
		PW_STREAM_FLAG_EARLY_PROCESS |
		PW_STREAM_FLAG_MAP_BUFFERS |
		/* This source owns the fixed 15-fps video clock.  The previous build
		 * only advertised node.driver as a property, while using the trigger
		 * flag combination intended for a different kind of stream.  Match the
		 * PipeWire provide-source scheduling model: a driver stream with a
		 * declared rate, kicked from the data path when it enters STREAMING. */
		PW_STREAM_FLAG_DRIVER;
	if (pw_stream_connect(data.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			stream_flags, params, 1) < 0) {
		fprintf(stderr, "cf20-pipewire-source: stream connect failed: %s\n",
			strerror(errno));
		goto cleanup_stream;
	}

	if (pthread_create(&data.reader, NULL, reader_main, &data) != 0)
		goto cleanup_stream;

	fprintf(stderr, "cf20-pipewire-source: publishing cf20-ov8858-raw\n");
	pw_main_loop_run(data.loop);
	atomic_store_explicit(&data.stop_reader, true, memory_order_relaxed);
	/* read(2) is a cancellation point; this also handles a stopped capture
	 * producer without relying on closing a descriptor from another thread. */
	pthread_cancel(data.reader);
	pthread_join(data.reader, NULL);
	result = 0;

cleanup_stream:
	if (data.stream != NULL)
		pw_stream_destroy(data.stream);
cleanup_core:
	if (data.core != NULL)
		pw_core_disconnect(data.core);
cleanup_context:
	if (data.context != NULL)
		pw_context_destroy(data.context);
cleanup_loop:
	if (data.loop != NULL)
		pw_main_loop_destroy(data.loop);
cleanup_mutex:
	if (mutex_ready)
		pthread_mutex_destroy(&data.frame_lock);
cleanup:
	free(data.frame);
	free(data.incoming);
	pw_deinit();
	return result;
}
