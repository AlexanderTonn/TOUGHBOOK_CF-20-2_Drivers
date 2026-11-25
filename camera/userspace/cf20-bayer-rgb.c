/* Convert the unpacked CF-20 OV8858 BGGR10 stream to a small RGB stream.
 *
 * cf20-ip3b-unpack emits one little-endian 16-bit Bayer sample per pixel.
 * The IPU3 ImgU/CSS path is deliberately disabled on this machine, so the
 * userspace bridge must perform the minimal demosaic and resize itself.
 *
 * The sensor format is SBGGR10_1X10: blue is at (even, even), red at
 * (odd, odd), and green at the other two positions.  The fixed 640x480
 * output keeps the PipeWire source predictable for desktop camera clients.
 * The CF-20 optical path presents the rear sensor horizontally mirrored, so
 * output pixels are sampled from right to left while demosaicing the source
 * at its original Bayer coordinates.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	INPUT_WIDTH = 1632,
	INPUT_HEIGHT = 1224,
	OUTPUT_WIDTH = 640,
	OUTPUT_HEIGHT = 480,
	INPUT_BYTES_PER_PIXEL = 2,
	OUTPUT_BYTES_PER_PIXEL = 3,
	MIRROR_HORIZONTAL = 1,
	BLACK_LEVEL = 64,
	WHITE_LEVEL = 959,
	WB_SAMPLE_STEP = 8,
};

struct white_balance {
	float red;
	float green;
	float blue;
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

static unsigned clamp_coord(int value, unsigned limit)
{
	if (value < 0)
		return 0;
	if ((unsigned)value >= limit)
		return limit - 1U;
	return (unsigned)value;
}

static unsigned sample10(const unsigned char *frame, int x, int y)
{
	size_t offset;

	x = clamp_coord(x, INPUT_WIDTH);
	y = clamp_coord(y, INPUT_HEIGHT);
	offset = ((size_t)y * INPUT_WIDTH + x) * INPUT_BYTES_PER_PIXEL;
	return (unsigned)frame[offset] | ((unsigned)frame[offset + 1U] << 8);
}

static unsigned remove_black_level(unsigned value)
{
	return value > BLACK_LEVEL ? value - BLACK_LEVEL : 0U;
}

static unsigned average2(unsigned a, unsigned b)
{
	return (a + b + 1U) / 2U;
}

static unsigned average4(unsigned a, unsigned b, unsigned c, unsigned d)
{
	return (a + b + c + d + 2U) / 4U;
}

static void demosaic(const unsigned char *frame, int x, int y,
		unsigned *red, unsigned *green, unsigned *blue)
{
	unsigned center = sample10(frame, x, y);

	if ((y & 1U) == 0U && (x & 1U) == 0U) {
		/* Blue pixel. */
		*blue = center;
		*green = average4(sample10(frame, x - 1, y),
			sample10(frame, x + 1, y),
			sample10(frame, x, y - 1),
			sample10(frame, x, y + 1));
		*red = average4(sample10(frame, x - 1, y - 1),
			sample10(frame, x + 1, y - 1),
			sample10(frame, x - 1, y + 1),
			sample10(frame, x + 1, y + 1));
		return;
	}

	if ((y & 1U) != 0U && (x & 1U) != 0U) {
		/* Red pixel. */
		*red = center;
		*green = average4(sample10(frame, x - 1, y),
			sample10(frame, x + 1, y),
			sample10(frame, x, y - 1),
			sample10(frame, x, y + 1));
		*blue = average4(sample10(frame, x - 1, y - 1),
			sample10(frame, x + 1, y - 1),
			sample10(frame, x - 1, y + 1),
			sample10(frame, x + 1, y + 1));
		return;
	}

	/* Green pixel.  The row parity identifies the horizontal colour pair. */
	*green = center;
	if ((y & 1U) == 0U) {
		*blue = average2(sample10(frame, x - 1, y),
			sample10(frame, x + 1, y));
		*red = average2(sample10(frame, x, y - 1),
			sample10(frame, x, y + 1));
	} else {
		*red = average2(sample10(frame, x - 1, y),
			sample10(frame, x + 1, y));
		*blue = average2(sample10(frame, x, y - 1),
			sample10(frame, x, y + 1));
	}
}

static unsigned apply_gain(unsigned value, float gain)
{
	float scaled = (float)remove_black_level(value) * gain;

	if (scaled <= 0.0f)
		return 0U;
	if (scaled >= (float)WHITE_LEVEL)
		return WHITE_LEVEL;
	return (unsigned)(scaled + 0.5f);
}

static unsigned to_u8(unsigned value)
{
	if (value > WHITE_LEVEL)
		value = WHITE_LEVEL;
	return (value * 255U + WHITE_LEVEL / 2U) / WHITE_LEVEL;
}

static float clamp_gain(float gain)
{
	if (gain < 0.65f)
		return 0.65f;
	if (gain > 1.8f)
		return 1.8f;
	return gain;
}

static void estimate_white_balance(const unsigned char *input,
		struct white_balance *balance)
{
	uint64_t red_sum = 0;
	uint64_t green_sum = 0;
	uint64_t blue_sum = 0;
	unsigned sample_count = 0;

	for (unsigned y = 0; y < OUTPUT_HEIGHT; y += WB_SAMPLE_STEP) {
		unsigned source_y = (y * INPUT_HEIGHT + OUTPUT_HEIGHT / 2U) /
			OUTPUT_HEIGHT;

		for (unsigned x = 0; x < OUTPUT_WIDTH; x += WB_SAMPLE_STEP) {
			unsigned source_x = (x * INPUT_WIDTH + OUTPUT_WIDTH / 2U) /
				OUTPUT_WIDTH;
			if (MIRROR_HORIZONTAL)
				source_x = INPUT_WIDTH - 1U - source_x;
			unsigned red;
			unsigned green;
			unsigned blue;
			unsigned luminance;

			demosaic(input, (int)source_x, (int)source_y,
				&red, &green, &blue);
			red = remove_black_level(red);
			green = remove_black_level(green);
			blue = remove_black_level(blue);
			luminance = (red + green + blue) / 3U;

			/* Ignore clipped and nearly-black samples so a scene does not
			 * turn the white balance into a hard color cast. */
			if (luminance < 32U || luminance > WHITE_LEVEL - 16U)
				continue;
			red_sum += red;
			green_sum += green;
			blue_sum += blue;
			sample_count++;
		}
	}

	if (sample_count < 16U || red_sum == 0U || green_sum == 0U ||
		blue_sum == 0U)
		return;

	float red_mean = (float)red_sum / (float)sample_count;
	float green_mean = (float)green_sum / (float)sample_count;
	float blue_mean = (float)blue_sum / (float)sample_count;
	float target = (red_mean + green_mean + blue_mean) / 3.0f;
	const float strength = 0.65f;
	float red_target = 1.0f + (target / red_mean - 1.0f) * strength;
	float green_target = 1.0f + (target / green_mean - 1.0f) * strength;
	float blue_target = 1.0f + (target / blue_mean - 1.0f) * strength;

	/* Smooth changes over frames to avoid visible flicker while the app
	 * is pointed at a changing scene. */
	red_target = clamp_gain(red_target);
	green_target = clamp_gain(green_target);
	blue_target = clamp_gain(blue_target);
	balance->red = balance->red * 0.85f + red_target * 0.15f;
	balance->green = balance->green * 0.85f + green_target * 0.15f;
	balance->blue = balance->blue * 0.85f + blue_target * 0.15f;
}

static void convert_frame(const unsigned char *input, unsigned char *output,
		struct white_balance *balance)
{
	estimate_white_balance(input, balance);

	for (unsigned y = 0; y < OUTPUT_HEIGHT; ++y) {
		unsigned source_y = (y * INPUT_HEIGHT + OUTPUT_HEIGHT / 2U) /
			OUTPUT_HEIGHT;
		unsigned char *row = output + (size_t)y * OUTPUT_WIDTH *
			OUTPUT_BYTES_PER_PIXEL;

		for (unsigned x = 0; x < OUTPUT_WIDTH; ++x) {
			unsigned source_x = (x * INPUT_WIDTH + OUTPUT_WIDTH / 2U) /
				OUTPUT_WIDTH;
			if (MIRROR_HORIZONTAL)
				source_x = INPUT_WIDTH - 1U - source_x;
			unsigned red;
			unsigned green;
			unsigned blue;
			size_t offset = (size_t)x * OUTPUT_BYTES_PER_PIXEL;

			demosaic(input, (int)source_x, (int)source_y, &red, &green, &blue);
			row[offset] = (unsigned char)to_u8(apply_gain(red, balance->red));
			row[offset + 1U] = (unsigned char)to_u8(apply_gain(green,
				balance->green));
			row[offset + 2U] = (unsigned char)to_u8(apply_gain(blue,
				balance->blue));
		}
	}
}

int main(void)
{
	const size_t input_size = (size_t)INPUT_WIDTH * INPUT_HEIGHT *
		INPUT_BYTES_PER_PIXEL;
	const size_t output_size = (size_t)OUTPUT_WIDTH * OUTPUT_HEIGHT *
		OUTPUT_BYTES_PER_PIXEL;
	struct white_balance balance = { .red = 1.0f, .green = 1.0f,
		.blue = 1.0f };
	unsigned char *input = malloc(input_size);
	unsigned char *output = malloc(output_size);

	if (input == NULL || output == NULL) {
		fprintf(stderr, "cf20-bayer-rgb: allocation failed\n");
		free(input);
		free(output);
		return 2;
	}

	for (;;) {
		ssize_t count = read_full(STDIN_FILENO, input, input_size);

		if (count == 0)
			break;
		if (count < 0) {
			fprintf(stderr, "cf20-bayer-rgb: read failed: %s\n",
				strerror(errno));
			free(input);
			free(output);
			return 1;
		}
		if ((size_t)count != input_size) {
			fprintf(stderr,
				"cf20-bayer-rgb: truncated frame (%zd of %zu bytes)\n",
				count, input_size);
			free(input);
			free(output);
			return 1;
		}

		convert_frame(input, output, &balance);
		if (write_full(STDOUT_FILENO, output, output_size) < 0) {
			if (errno == EPIPE)
				break;
			fprintf(stderr, "cf20-bayer-rgb: write failed: %s\n",
				strerror(errno));
			free(input);
			free(output);
			return 1;
		}
	}

	free(input);
	free(output);
	return 0;
}
