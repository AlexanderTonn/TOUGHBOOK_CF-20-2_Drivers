#define _POSIX_C_SOURCE 200809L

/*
 * Minimal raw V4L2 MMAP capturer for the CF-20 IPU3 CIO2 node.
 *
 * v4l2-ctl is useful for diagnostics and regular-file captures, but its
 * stdout path is not usable on this target.  This helper keeps the kernel
 * path identical and writes each complete ip3b plane with write(2), so a
 * pipe can never lose stdio buffering or progress output.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

enum {
	WIDTH = 1632,
	HEIGHT = 1224,
	STRIDE = 2112,
	FRAME_BYTES = STRIDE * HEIGHT,
	/* Match v4l2-ctl's default --stream-mmap request on this driver. */
	REQUESTED_BUFFER_COUNT = 3,
	MAX_BUFFER_COUNT = 8,
};

#define IPU3_SBGGR10 v4l2_fourcc('i', 'p', '3', 'b')

struct mapped_buffer {
	void *address;
	size_t length;
};

static volatile sig_atomic_t stopping;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR && !stopping);

	return result;
}

static int write_full(const unsigned char *data, size_t length)
{
	size_t written = 0;

	while (written < length && !stopping) {
		ssize_t result = write(STDOUT_FILENO, data + written, length - written);

		if (result > 0) {
			written += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0 && errno == EPIPE) {
			stopping = 1;
			return 0;
		}
		return -1;
	}

	return stopping ? 0 : 1;
}

static void unmap_buffers(struct mapped_buffer *buffers, unsigned int count)
{
	for (unsigned int index = 0; index < count; ++index) {
		if (buffers[index].address && buffers[index].address != MAP_FAILED)
			munmap(buffers[index].address, buffers[index].length);
		buffers[index].address = NULL;
		buffers[index].length = 0;
	}
}

int main(int argc, char **argv)
{
	const char *device = argc > 1 ? argv[1] : "/dev/video0";
	struct mapped_buffer buffers[MAX_BUFFER_COUNT] = { 0 };
	struct v4l2_format format = { 0 };
	struct v4l2_requestbuffers request = { 0 };
	struct sigaction action = { 0 };
	unsigned int buffer_count = 0;
	bool stream_started = false;
	int fd = -1;
	int result = EXIT_FAILURE;

	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
		sigaction(SIGTERM, &action, NULL) < 0) {
		fprintf(stderr, "cf20-cio2-capture: sigaction failed: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	/*
	 * Keep the descriptor blocking.  v4l2-ctl's known-good path on the
	 * CF-20 opens the node this way and performs a blocking DQBUF.  The
	 * non-blocking/poll path is valid V4L2, but this CIO2 build has returned
	 * Remote I/O on that path even though the blocking path delivers frames.
	 * Signals still interrupt DQBUF because the handler is installed without
	 * SA_RESTART.
	 */
	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "cf20-cio2-capture: cannot open %s: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}

	struct v4l2_capability capability = { 0 };
	if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
		fprintf(stderr, "cf20-cio2-capture: VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
		goto cleanup;
	}
	if (!(capability.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
		!(capability.capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "cf20-cio2-capture: device lacks multiplanar streaming capture\n");
		goto cleanup;
	}

	/*
	 * Match v4l2-ctl's capture_setup(): stop any stale queue and release
	 * previously allocated MMAP buffers before requesting a fresh queue.  This
	 * matters on the CF-20 because the PipeWire service may be restarted after
	 * a failed STREAMON, while the CIO2 driver keeps queue state in the video
	 * device until REQBUFS(0).  STREAMOFF is expected to return EINVAL when no
	 * stream is active; any other error is real and must stop before hardware
	 * access.
	 */
	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		struct v4l2_requestbuffers release = {
			.count = 0,
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
		};

		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0 && errno != EINVAL) {
			fprintf(stderr, "cf20-cio2-capture: initial VIDIOC_STREAMOFF failed: %s\n",
				strerror(errno));
			goto cleanup;
		}
		if (xioctl(fd, VIDIOC_REQBUFS, &release) < 0) {
			fprintf(stderr, "cf20-cio2-capture: initial VIDIOC_REQBUFS(0) failed: %s\n",
				strerror(errno));
			goto cleanup;
		}
	}

	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	/*
	 * The known-good CF-20 sequence sets this format before streaming (the
	 * kernel initializes the rear queue to it, and v4l2-ctl can set it in a
	 * separate ioctl call).  Do not issue S_FMT here: this firmware/driver
	 * combination can accept that ioctl but then return -EREMOTEIO from the
	 * following STREAMON, while the same already-negotiated format streams
	 * successfully through v4l2-ctl.  Keep this helper read-only with respect
	 * to format negotiation and fail before REQBUFS if the queue is wrong.
	 */
	if (xioctl(fd, VIDIOC_G_FMT, &format) < 0) {
		fprintf(stderr, "cf20-cio2-capture: initial VIDIOC_G_FMT failed: %s\n",
			strerror(errno));
		goto cleanup;
	}
	if (format.fmt.pix_mp.width != WIDTH ||
		format.fmt.pix_mp.height != HEIGHT ||
		format.fmt.pix_mp.pixelformat != IPU3_SBGGR10 ||
		format.fmt.pix_mp.num_planes != 1) {
		fprintf(stderr,
			"cf20-cio2-capture: current queue has unsupported format %ux%u fourcc=0x%08x planes=%u\n",
			format.fmt.pix_mp.width, format.fmt.pix_mp.height,
			format.fmt.pix_mp.pixelformat, format.fmt.pix_mp.num_planes);
		goto cleanup;
	}
	if (format.fmt.pix_mp.plane_fmt[0].bytesperline != STRIDE ||
		format.fmt.pix_mp.plane_fmt[0].sizeimage < FRAME_BYTES) {
		fprintf(stderr,
			"cf20-cio2-capture: unexpected stride/size: stride=%u size=%u\n",
			format.fmt.pix_mp.plane_fmt[0].bytesperline,
			format.fmt.pix_mp.plane_fmt[0].sizeimage);
		goto cleanup;
	}

	request.count = REQUESTED_BUFFER_COUNT;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	request.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
		fprintf(stderr, "cf20-cio2-capture: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
		goto cleanup;
	}
	if (request.count > MAX_BUFFER_COUNT) {
		fprintf(stderr, "cf20-cio2-capture: driver returned too many buffers: %u\n",
			request.count);
		goto cleanup;
	}
	buffer_count = request.count;

	/* v4l2-ctl refreshes the negotiated format after REQBUFS. */
	if (xioctl(fd, VIDIOC_G_FMT, &format) < 0) {
		fprintf(stderr, "cf20-cio2-capture: VIDIOC_G_FMT failed: %s\n", strerror(errno));
		goto cleanup;
	}

	for (unsigned int index = 0; index < buffer_count; ++index) {
		struct v4l2_buffer buffer = { 0 };
		struct v4l2_plane plane = { 0 };

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		buffer.length = 1;
		buffer.m.planes = &plane;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			fprintf(stderr, "cf20-cio2-capture: VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
			goto cleanup;
		}
		buffers[index].length = plane.length;
		buffers[index].address = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, plane.m.mem_offset);
		if (buffers[index].address == MAP_FAILED) {
			fprintf(stderr, "cf20-cio2-capture: mmap failed: %s\n", strerror(errno));
			buffers[index].address = NULL;
			goto cleanup;
		}
	}

	for (unsigned int index = 0; index < buffer_count; ++index) {
		struct v4l2_buffer buffer = { 0 };
		struct v4l2_plane plane = { 0 };

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		buffer.length = 1;
		/* Preserve the plane description returned by QUERYBUF. */
		plane.length = buffers[index].length;
		buffer.m.planes = &plane;
		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			fprintf(stderr, "cf20-cio2-capture: VIDIOC_QBUF failed: %s\n", strerror(errno));
			goto cleanup;
		}
	}

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
			fprintf(stderr, "cf20-cio2-capture: VIDIOC_STREAMON failed: %s\n", strerror(errno));
			goto cleanup;
		}
		stream_started = true;
	}

	while (!stopping) {
		struct v4l2_buffer buffer = { 0 };
		struct v4l2_plane plane = { 0 };
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.length = 1;
		/* DQBUF also requires a filled-in multi-planar array. */
		plane.length = buffers[0].length;
		buffer.m.planes = &plane;
		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EINTR && stopping)
				break;
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "cf20-cio2-capture: VIDIOC_DQBUF failed: %s\n", strerror(errno));
			break;
		}
		if (buffer.index >= buffer_count || plane.bytesused < FRAME_BYTES) {
			fprintf(stderr,
				"cf20-cio2-capture: unexpected frame index=%u bytes=%u minimum=%u\n",
				buffer.index, plane.bytesused, FRAME_BYTES);
			break;
		}
		if (write_full(buffers[buffer.index].address, FRAME_BYTES) < 0) {
			fprintf(stderr, "cf20-cio2-capture: stdout write failed: %s\n", strerror(errno));
			break;
		}
		/* Requeue with a fresh plane descriptor, like v4l2-ctl. */
		unsigned int buffer_index = buffer.index;
		memset(&buffer, 0, sizeof(buffer));
		memset(&plane, 0, sizeof(plane));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = buffer_index;
		buffer.length = 1;
		plane.length = buffers[buffer_index].length;
		buffer.m.planes = &plane;
		if (!stopping && xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			fprintf(stderr, "cf20-cio2-capture: VIDIOC_QBUF failed: %s\n", strerror(errno));
			break;
		}
	}

	result = stopping ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
	if (fd >= 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		struct v4l2_requestbuffers release = {
			.count = 0,
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
		};

		if (stream_started)
			xioctl(fd, VIDIOC_STREAMOFF, &type);
		unmap_buffers(buffers, buffer_count);
		if (buffer_count)
			xioctl(fd, VIDIOC_REQBUFS, &release);
	} else {
		unmap_buffers(buffers, buffer_count);
	}
	if (fd >= 0)
		close(fd);
	return result;
}
