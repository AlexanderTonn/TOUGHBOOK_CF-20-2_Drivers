/*
 * Convert Intel IPU3 V4L2_PIX_FMT_IPU3_SBGGR10 ("ip3b") frames to the
 * unpacked 16-bit Bayer format accepted by GStreamer's bayer2rgb element.
 *
 * The IPU3 format stores 25 ten-bit pixels in 32 bytes, little-endian bit
 * packed.  The final six bits of each block are padding.  The input is a
 * sequence of complete, tightly packed frames with a possibly padded stride;
 * the output is one contiguous little-endian uint16 value per pixel.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	WIDTH = 1632,
	HEIGHT = 1224,
	STRIDE = 2112,
	BLOCK_BYTES = 32,
	PIXELS_PER_BLOCK = 25,
	OUTPUT_BYTES_PER_PIXEL = 2,
};

static ssize_t read_full(int fd, unsigned char *buffer, size_t length)
{
	size_t done = 0;

	while (done < length) {
		ssize_t count = read(fd, buffer + done, length - done);

		if (count > 0) {
			done += (size_t)count;
			continue;
		}
		if (count == 0)
			break;
		if (errno == EINTR)
			continue;
		return -1;
	}

	return (ssize_t)done;
}

static int write_full(int fd, const unsigned char *buffer, size_t length)
{
	size_t done = 0;

	while (done < length) {
		ssize_t count = write(fd, buffer + done, length - done);

		if (count > 0) {
			done += (size_t)count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		return -1;
	}

	return 0;
}

static uint16_t unpack_pixel(const unsigned char *block, unsigned int pixel)
{
	unsigned int bit = pixel * 10U;
	unsigned int byte = bit / 8U;
	unsigned int shift = bit % 8U;
	uint32_t word = (uint32_t)block[byte]
		| ((uint32_t)block[byte + 1U] << 8)
		| ((uint32_t)block[byte + 2U] << 16);

	return (uint16_t)((word >> shift) & 0x3ffU);
}

static int convert_frame(const unsigned char *input, unsigned char *output)
{
	for (unsigned int y = 0; y < HEIGHT; ++y) {
		const unsigned char *row = input + (size_t)y * STRIDE;
		unsigned char *out = output + (size_t)y * WIDTH * OUTPUT_BYTES_PER_PIXEL;

		for (unsigned int block = 0; block < STRIDE / BLOCK_BYTES; ++block) {
			const unsigned char *packed = row + (size_t)block * BLOCK_BYTES;
			unsigned int first = block * PIXELS_PER_BLOCK;
			unsigned int count = WIDTH - first;

			if (count > PIXELS_PER_BLOCK)
				count = PIXELS_PER_BLOCK;
			if (count == 0)
				break;

			for (unsigned int pixel = 0; pixel < count; ++pixel) {
				uint16_t value = unpack_pixel(packed, pixel);
				unsigned int offset = (first + pixel) * OUTPUT_BYTES_PER_PIXEL;

				out[offset] = (unsigned char)(value & 0xffU);
				out[offset + 1U] = (unsigned char)(value >> 8);
			}
		}
	}

	return 0;
}

int main(void)
{
	const size_t input_size = (size_t)STRIDE * HEIGHT;
	const size_t output_size = (size_t)WIDTH * HEIGHT * OUTPUT_BYTES_PER_PIXEL;
	unsigned char *input = malloc(input_size);
	unsigned char *output = malloc(output_size);

	if (!input || !output) {
		fprintf(stderr, "cf20-ip3b-unpack: allocation failed\n");
		free(input);
		free(output);
		return 2;
	}

	for (;;) {
		ssize_t count = read_full(STDIN_FILENO, input, input_size);

		if (count == 0)
			break;
		if (count < 0) {
			fprintf(stderr, "cf20-ip3b-unpack: read failed: %s\n", strerror(errno));
			free(input);
			free(output);
			return 1;
		}
		if ((size_t)count != input_size) {
			fprintf(stderr,
				"cf20-ip3b-unpack: truncated frame (%zd of %zu bytes)\n",
				count, input_size);
			free(input);
			free(output);
			return 1;
		}

		convert_frame(input, output);
		if (write_full(STDOUT_FILENO, output, output_size) < 0) {
			if (errno == EPIPE)
				break;
			fprintf(stderr, "cf20-ip3b-unpack: write failed: %s\n", strerror(errno));
			free(input);
			free(output);
			return 1;
		}
	}

	free(input);
	free(output);
	return 0;
}
