// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded V4L2 mem2mem JPEG qualification for the Gemini PDA.
 *
 * This intentionally uses no MediaTek-private API.  It encodes one synthetic
 * YUYV frame, checks the JPEG envelope, then decodes that exact output.
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define TEST_WIDTH 64U
#define TEST_HEIGHT 64U
#define MAX_PLANES VIDEO_MAX_PLANES
#define DEFAULT_TIMEOUT_MS 3000
#define MIN_TIMEOUT_MS 100
#define MAX_TIMEOUT_MS 30000
#define JPEG_BUFFER_SIZE (1024U * 1024U)

struct mapped_plane {
	void *addr;
	size_t length;
};

struct mapped_queue {
	enum v4l2_buf_type type;
	unsigned int num_planes;
	struct mapped_plane planes[MAX_PLANES];
	bool allocated;
	bool streaming;
};

struct options {
	const char *encoder;
	const char *decoder;
	int timeout_ms;
	bool selftest;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return -1;

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_for(int fd, short events, int timeout_ms, const char *what)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = events,
	};
	int64_t start = monotonic_ms();
	int64_t deadline;

	if (start < 0) {
		perror("clock_gettime");
		return -1;
	}
	deadline = start + timeout_ms;

	for (;;) {
		int64_t now = monotonic_ms();
		int remaining;
		int ret;

		if (now < 0)
			return -1;
		remaining = deadline > now ? (int)(deadline - now) : 0;
		ret = poll(&pfd, 1, remaining);
		if (ret > 0) {
			if (pfd.revents & events)
				return 0;
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				fprintf(stderr, "%s: poll error events %#x\n",
					what, pfd.revents);
				errno = EIO;
				return -1;
			}
			continue;
		}
		if (!ret) {
			fprintf(stderr, "%s: timed out after %d ms\n",
				what, timeout_ms);
			errno = ETIMEDOUT;
			return -1;
		}
		if (errno != EINTR) {
			perror(what);
			return -1;
		}
	}
}

static bool path_has_component(const char *path, const char *component)
{
	size_t length = strlen(component);
	const char *match = path;

	while ((match = strstr(match, component))) {
		if ((match == path || match[-1] == '/') &&
		    (match[length] == '/' || match[length] == '\0'))
			return true;
		match += length;
	}
	return false;
}

static int require_platform_ancestor(int fd, const char *path,
				     const char *expected)
{
	const char *sysfs_root = getenv("SYSFS_ROOT");
	struct stat st;
	char device_path[PATH_MAX];
	char resolved[PATH_MAX];
	int length;

	if (fstat(fd, &st)) {
		perror("fstat video device");
		return -1;
	}
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s: not a character device\n", path);
		errno = ENODEV;
		return -1;
	}
	if (!sysfs_root || !sysfs_root[0])
		sysfs_root = "/sys";
	length = snprintf(device_path, sizeof(device_path),
			  "%s/dev/char/%u:%u", sysfs_root,
			  major(st.st_rdev), minor(st.st_rdev));
	if (length < 0 || (size_t)length >= sizeof(device_path)) {
		fprintf(stderr, "%s: sysfs path is too long\n", path);
		errno = ENAMETOOLONG;
		return -1;
	}
	if (!realpath(device_path, resolved)) {
		perror(device_path);
		return -1;
	}
	if (!path_has_component(resolved, expected)) {
		fprintf(stderr, "%s: sysfs ancestry is %s, expected %s\n",
			path, resolved, expected);
		errno = ENODEV;
		return -1;
	}
	printf("%s: platform ancestor %s\n", path, expected);
	return 0;
}

static const char *fourcc_name(uint32_t fourcc, char name[5])
{
	name[0] = fourcc & 0xff;
	name[1] = (fourcc >> 8) & 0xff;
	name[2] = (fourcc >> 16) & 0xff;
	name[3] = (fourcc >> 24) & 0xff;
	name[4] = '\0';
	return name;
}

static int require_m2m_device(int fd, const char *path, const char *driver,
			      const char *platform_device)
{
	struct v4l2_capability cap = {};
	uint32_t caps;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		perror("VIDIOC_QUERYCAP");
		return -1;
	}
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ?
		cap.device_caps : cap.capabilities;
	if (!(caps & V4L2_CAP_STREAMING) ||
	    !(caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
		fprintf(stderr, "%s: not a streaming multiplanar mem2mem node\n",
			path);
		errno = ENODEV;
		return -1;
	}
	if (strcmp((const char *)cap.driver, driver)) {
		fprintf(stderr, "%s: driver is '%s', expected '%s'\n", path,
			cap.driver, driver);
		errno = ENODEV;
		return -1;
	}
	if (require_platform_ancestor(fd, path, platform_device))
		return -1;

	return 0;
}

static int set_format(int fd, enum v4l2_buf_type type, uint32_t fourcc,
		      uint32_t width, uint32_t height, uint32_t sizeimage,
		      struct v4l2_format *result)
{
	struct v4l2_format fmt = {
		.type = type,
	};
	char got[5];
	char wanted[5];

	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = fourcc;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt)) {
		perror("VIDIOC_S_FMT");
		return -1;
	}
	if (fmt.fmt.pix_mp.pixelformat != fourcc) {
		fprintf(stderr, "format changed from %s to %s\n",
			fourcc_name(fourcc, wanted),
			fourcc_name(fmt.fmt.pix_mp.pixelformat, got));
		errno = EINVAL;
		return -1;
	}
	if (!fmt.fmt.pix_mp.num_planes ||
	    fmt.fmt.pix_mp.num_planes > MAX_PLANES) {
		fprintf(stderr, "invalid plane count %u\n",
			fmt.fmt.pix_mp.num_planes);
		errno = EINVAL;
		return -1;
	}
	*result = fmt;
	return 0;
}

static int get_format(int fd, enum v4l2_buf_type type,
		      struct v4l2_format *result)
{
	struct v4l2_format fmt = {
		.type = type,
	};

	if (xioctl(fd, VIDIOC_G_FMT, &fmt)) {
		perror("VIDIOC_G_FMT");
		return -1;
	}
	if (!fmt.fmt.pix_mp.num_planes ||
	    fmt.fmt.pix_mp.num_planes > MAX_PLANES) {
		fprintf(stderr, "invalid plane count %u\n",
			fmt.fmt.pix_mp.num_planes);
		errno = EINVAL;
		return -1;
	}
	*result = fmt;
	return 0;
}

static int alloc_queue(int fd, struct mapped_queue *queue,
		       const struct v4l2_format *fmt)
{
	struct v4l2_requestbuffers req = {
		.count = 1,
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
	};
	struct v4l2_plane planes[MAX_PLANES] = {};
	struct v4l2_buffer buf = {
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.index = 0,
		.length = fmt->fmt.pix_mp.num_planes,
		.m.planes = planes,
	};
	unsigned int i;

	if (xioctl(fd, VIDIOC_REQBUFS, &req)) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}
	if (req.count < 1) {
		fprintf(stderr, "driver allocated no buffers\n");
		errno = ENOMEM;
		return -1;
	}
	queue->allocated = true;
	queue->num_planes = buf.length;
	if (xioctl(fd, VIDIOC_QUERYBUF, &buf)) {
		perror("VIDIOC_QUERYBUF");
		return -1;
	}
	if (buf.length != queue->num_planes) {
		fprintf(stderr, "VIDIOC_QUERYBUF changed plane count %u to %u\n",
			queue->num_planes, buf.length);
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < queue->num_planes; i++) {
		if (!planes[i].length) {
			fprintf(stderr, "plane %u has zero length\n", i);
			errno = EINVAL;
			return -1;
		}
		queue->planes[i].length = planes[i].length;
		queue->planes[i].addr =
			mmap(NULL, planes[i].length, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, planes[i].m.mem_offset);
		if (queue->planes[i].addr == MAP_FAILED) {
			queue->planes[i].addr = NULL;
			perror("mmap");
			return -1;
		}
	}

	return 0;
}

static int queue_buffer(int fd, const struct mapped_queue *queue,
			const size_t bytesused[MAX_PLANES])
{
	struct v4l2_plane planes[MAX_PLANES] = {};
	struct v4l2_buffer buf = {
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.index = 0,
		.length = queue->num_planes,
		.m.planes = planes,
	};
	unsigned int i;

	for (i = 0; i < queue->num_planes; i++) {
		if (bytesused[i] > queue->planes[i].length) {
			fprintf(stderr, "plane %u payload exceeds buffer\n", i);
			errno = EOVERFLOW;
			return -1;
		}
		planes[i].bytesused = bytesused[i];
		planes[i].length = queue->planes[i].length;
	}
	if (xioctl(fd, VIDIOC_QBUF, &buf)) {
		perror("VIDIOC_QBUF");
		return -1;
	}
	return 0;
}

static int stream_on(int fd, struct mapped_queue *queue)
{
	enum v4l2_buf_type type = queue->type;

	if (xioctl(fd, VIDIOC_STREAMON, &type)) {
		perror("VIDIOC_STREAMON");
		return -1;
	}
	queue->streaming = true;
	return 0;
}

static int dequeue_buffer(int fd, const struct mapped_queue *queue,
			  int timeout_ms, size_t bytesused[MAX_PLANES])
{
	struct v4l2_plane planes[MAX_PLANES] = {};
	struct v4l2_buffer buf = {
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.length = queue->num_planes,
		.m.planes = planes,
	};
	short events = V4L2_TYPE_IS_OUTPUT(queue->type) ? POLLOUT : POLLIN;
	const char *what = V4L2_TYPE_IS_OUTPUT(queue->type) ?
		"output dequeue" : "capture dequeue";
	int64_t start = monotonic_ms();
	int64_t deadline;
	unsigned int i;

	if (start < 0) {
		perror("clock_gettime");
		return -1;
	}
	deadline = start + timeout_ms;

	for (;;) {
		int64_t now;
		int remaining;

		if (!xioctl(fd, VIDIOC_DQBUF, &buf))
			break;
		if (errno != EAGAIN) {
			perror("VIDIOC_DQBUF");
			return -1;
		}
		now = monotonic_ms();
		if (now < 0)
			return -1;
		remaining = deadline > now ? (int)(deadline - now) : 0;
		if (wait_for(fd, events, remaining, what))
			return -1;
	}
	if (buf.index != 0 || buf.length != queue->num_planes ||
	    buf.flags & V4L2_BUF_FLAG_ERROR) {
		fprintf(stderr, "%s: bad completion index=%u planes=%u flags=%#x\n",
			what, buf.index, buf.length, buf.flags);
		errno = EIO;
		return -1;
	}
	for (i = 0; i < queue->num_planes; i++) {
		if (planes[i].bytesused > queue->planes[i].length) {
			fprintf(stderr, "%s: plane %u bytesused overflow\n", what, i);
			errno = EOVERFLOW;
			return -1;
		}
		if (planes[i].data_offset) {
			fprintf(stderr, "%s: plane %u has unsupported data offset %u\n",
				what, i, planes[i].data_offset);
			errno = EPROTO;
			return -1;
		}
		bytesused[i] = planes[i].bytesused;
	}
	return 0;
}

static int stream_off(int fd, struct mapped_queue *queue)
{
	enum v4l2_buf_type type = queue->type;
	int ret = 0;

	if (!queue->streaming)
		return 0;
	if (xioctl(fd, VIDIOC_STREAMOFF, &type)) {
		perror("VIDIOC_STREAMOFF");
		ret = -1;
	}
	queue->streaming = false;
	return ret;
}

static int release_queue(int fd, struct mapped_queue *queue)
{
	struct v4l2_requestbuffers req = {
		.count = 0,
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;
	int ret = 0;

	if (stream_off(fd, queue))
		ret = -1;
	for (i = 0; i < queue->num_planes; i++) {
		if (queue->planes[i].addr) {
			if (munmap(queue->planes[i].addr,
				   queue->planes[i].length)) {
				perror("munmap");
				ret = -1;
			}
			queue->planes[i].addr = NULL;
		}
	}
	if (queue->allocated) {
		if (xioctl(fd, VIDIOC_REQBUFS, &req)) {
			perror("VIDIOC_REQBUFS(count=0)");
			ret = -1;
		}
		queue->allocated = false;
	}
	return ret;
}

static void fill_yuyv(uint8_t *data, uint32_t width, uint32_t height,
		      uint32_t stride)
{
	uint32_t x;
	uint32_t y;

	for (y = 0; y < height; y++) {
		uint8_t *row = data + y * stride;

		for (x = 0; x < width; x += 2) {
			row[x * 2] = 32 + ((x * 3 + y * 5) % 192);
			row[x * 2 + 1] = 96 + ((x + y) % 64);
			row[x * 2 + 2] = 32 + ((x * 7 + y * 2) % 192);
			row[x * 2 + 3] = 96 + ((2 * x + y) % 64);
		}
	}
}

static uint16_t read_be16(const uint8_t *data)
{
	return ((uint16_t)data[0] << 8) | data[1];
}

static bool valid_jpeg(const uint8_t *data, size_t length,
		       uint16_t width, uint16_t height)
{
	size_t offset = 2;
	bool saw_frame = false;

	if (length < 4 || data[0] != 0xff || data[1] != 0xd8)
		return false;

	while (offset < length) {
		uint16_t segment_length;
		uint8_t marker;

		if (data[offset++] != 0xff)
			return false;
		while (offset < length && data[offset] == 0xff)
			offset++;
		if (offset >= length)
			return false;
		marker = data[offset++];
		if (!marker || marker == 0xd8 || marker == 0xd9 ||
		    marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
			return false;
		if (offset + 2 > length)
			return false;
		segment_length = read_be16(data + offset);
		if (segment_length < 2 || segment_length > length - offset)
			return false;

		if (marker == 0xc0) {
			uint8_t components;

			if (saw_frame || segment_length < 8 ||
			    data[offset + 2] != 8 ||
			    read_be16(data + offset + 3) != height ||
			    read_be16(data + offset + 5) != width)
				return false;
			components = data[offset + 7];
			if (components != 3 ||
			    segment_length != 8 + 3 * components)
				return false;
			saw_frame = true;
		} else if ((marker >= 0xc1 && marker <= 0xcf) &&
			   marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			/* This bounded test requires the encoder's baseline frame. */
			return false;
		}

		offset += segment_length;
		if (marker == 0xda) {
			if (!saw_frame)
				return false;
			while (offset < length) {
				if (data[offset++] != 0xff)
					continue;
				while (offset < length && data[offset] == 0xff)
					offset++;
				if (offset >= length)
					return false;
				marker = data[offset++];
				if (!marker || (marker >= 0xd0 && marker <= 0xd7))
					continue;
				return marker == 0xd9 && offset == length;
			}
			return false;
		}
	}

	return false;
}

static uint32_t checksum(const uint8_t *data, size_t length)
{
	uint32_t hash = 2166136261U;
	size_t i;

	for (i = 0; i < length; i++) {
		hash ^= data[i];
		hash *= 16777619U;
	}
	return hash;
}

static bool active_plane_checksum(const uint8_t *data, size_t bytesused,
				  size_t stride, size_t width, size_t height,
				  uint32_t *result)
{
	uint32_t hash = 2166136261U;
	uint8_t first;
	bool varied = false;
	size_t required;
	size_t x;
	size_t y;

	if (!width || !height || stride < width ||
	    height - 1 > (SIZE_MAX - width) / stride)
		return false;
	required = (height - 1) * stride + width;
	if (bytesused < required)
		return false;
	first = data[0];
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			uint8_t value = data[y * stride + x];

			varied |= value != first;
			hash ^= value;
			hash *= 16777619U;
		}
	}
	*result = hash;
	return varied;
}

static void decoded_plane_geometry(uint32_t fourcc, uint32_t width,
				   uint32_t height, unsigned int plane,
				   size_t *plane_width, size_t *plane_height)
{
	if (!plane) {
		*plane_width = width;
		*plane_height = height;
		return;
	}
	*plane_width = (width + 1) / 2;
	*plane_height = fourcc == V4L2_PIX_FMT_YUV420M ?
		(height + 1) / 2 : height;
}

static int encode_one(const struct options *opts, uint8_t **jpeg_data,
		      size_t *jpeg_size)
{
	struct mapped_queue output = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	};
	struct mapped_queue capture = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	};
	struct v4l2_format output_fmt;
	struct v4l2_format capture_fmt;
	struct v4l2_control quality = {
		.id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
		.value = 90,
	};
	size_t output_used[MAX_PLANES] = {};
	size_t capture_used[MAX_PLANES] = {};
	int fd = -1;
	int ret = -1;

	fd = open(opts->encoder, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror(opts->encoder);
		goto out;
	}
	if (require_m2m_device(fd, opts->encoder, "mtk-jpeg-enc",
			       "17003000.jpeg-encoder"))
		goto out;
	if (set_format(fd, output.type, V4L2_PIX_FMT_YUYV,
		       TEST_WIDTH, TEST_HEIGHT, 0, &output_fmt) ||
	    set_format(fd, capture.type, V4L2_PIX_FMT_JPEG,
		       TEST_WIDTH, TEST_HEIGHT, JPEG_BUFFER_SIZE, &capture_fmt))
		goto out;
	if (output_fmt.fmt.pix_mp.num_planes != 1 ||
	    capture_fmt.fmt.pix_mp.num_planes != 1) {
		fprintf(stderr, "encoder returned an unexpected plane layout\n");
		goto out;
	}
	if (output_fmt.fmt.pix_mp.width != TEST_WIDTH ||
	    output_fmt.fmt.pix_mp.height != TEST_HEIGHT ||
	    capture_fmt.fmt.pix_mp.width != TEST_WIDTH ||
	    capture_fmt.fmt.pix_mp.height != TEST_HEIGHT) {
		fprintf(stderr, "encoder changed the requested 64x64 dimensions\n");
		goto out;
	}
	if (output_fmt.fmt.pix_mp.plane_fmt[0].bytesperline <
	    TEST_WIDTH * 2 ||
	    output_fmt.fmt.pix_mp.plane_fmt[0].sizeimage <
	    (size_t)output_fmt.fmt.pix_mp.plane_fmt[0].bytesperline *
	    TEST_HEIGHT) {
		fprintf(stderr, "encoder returned an undersized YUYV layout\n");
		goto out;
	}
	if (xioctl(fd, VIDIOC_S_CTRL, &quality)) {
		perror("VIDIOC_S_CTRL(JPEG quality)");
		goto out;
	}
	if (alloc_queue(fd, &output, &output_fmt) ||
	    alloc_queue(fd, &capture, &capture_fmt))
		goto out;
	memset(capture.planes[0].addr, 0xa5, capture.planes[0].length);
	output_used[0] = output_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (output_used[0] > output.planes[0].length) {
		fprintf(stderr, "encoder output format exceeds mapped buffer\n");
		goto out;
	}
	memset(output.planes[0].addr, 0, output_used[0]);
	fill_yuyv(output.planes[0].addr, output_fmt.fmt.pix_mp.width,
		  output_fmt.fmt.pix_mp.height,
		  output_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
	if (queue_buffer(fd, &capture, capture_used) ||
	    queue_buffer(fd, &output, output_used) ||
	    stream_on(fd, &capture) || stream_on(fd, &output) ||
	    dequeue_buffer(fd, &capture, opts->timeout_ms, capture_used) ||
	    dequeue_buffer(fd, &output, opts->timeout_ms, output_used))
		goto out;
	if (!valid_jpeg(capture.planes[0].addr, capture_used[0],
			TEST_WIDTH, TEST_HEIGHT)) {
		fprintf(stderr, "encoder output is not a baseline 64x64 JPEG\n");
		goto out;
	}
	if (output_used[0] !=
	    output_fmt.fmt.pix_mp.plane_fmt[0].sizeimage) {
		fprintf(stderr, "encoder returned a short input buffer: %zu\n",
			output_used[0]);
		goto out;
	}
	*jpeg_data = malloc(capture_used[0]);
	if (!*jpeg_data) {
		perror("malloc JPEG");
		goto out;
	}
	memcpy(*jpeg_data, capture.planes[0].addr, capture_used[0]);
	*jpeg_size = capture_used[0];
	ret = 0;
out:
	if (fd >= 0) {
		int cleanup_failed = 0;

		cleanup_failed |= release_queue(fd, &capture) != 0;
		cleanup_failed |= release_queue(fd, &output) != 0;
		if (close(fd)) {
			perror("close encoder");
			cleanup_failed = 1;
		}
		if (!ret && cleanup_failed)
			ret = -1;
	}
	return ret;
}

static int wait_source_change(int fd, int timeout_ms)
{
	struct v4l2_event event = {};

	if (wait_for(fd, POLLPRI, timeout_ms, "decoder source change"))
		return -1;
	if (xioctl(fd, VIDIOC_DQEVENT, &event)) {
		perror("VIDIOC_DQEVENT");
		return -1;
	}
	if (event.type != V4L2_EVENT_SOURCE_CHANGE ||
	    !(event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) {
		fprintf(stderr, "unexpected decoder event type=%u changes=%#x\n",
			event.type, event.u.src_change.changes);
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static int decode_one(const struct options *opts, const uint8_t *jpeg_data,
		      size_t jpeg_size)
{
	struct mapped_queue output = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	};
	struct mapped_queue capture = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	};
	struct v4l2_event_subscription sub = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
	};
	struct v4l2_format output_fmt;
	struct v4l2_format capture_fmt;
	size_t output_used[MAX_PLANES] = {};
	size_t capture_used[MAX_PLANES] = {};
	char format_name[5];
	unsigned int i;
	int fd = -1;
	int ret = -1;

	fd = open(opts->decoder, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror(opts->decoder);
		goto out;
	}
	if (require_m2m_device(fd, opts->decoder, "mtk-jpeg-dec",
			       "17004000.jpeg-decoder"))
		goto out;
	if (set_format(fd, output.type, V4L2_PIX_FMT_JPEG, TEST_WIDTH,
		       TEST_HEIGHT, jpeg_size, &output_fmt))
		goto out;
	if (output_fmt.fmt.pix_mp.num_planes != 1 ||
	    output_fmt.fmt.pix_mp.width != TEST_WIDTH ||
	    output_fmt.fmt.pix_mp.height != TEST_HEIGHT ||
	    output_fmt.fmt.pix_mp.plane_fmt[0].sizeimage < jpeg_size) {
		fprintf(stderr, "decoder returned an invalid JPEG input layout\n");
		goto out;
	}
	if (alloc_queue(fd, &output, &output_fmt))
		goto out;
	if (jpeg_size > output.planes[0].length) {
		fprintf(stderr, "encoded JPEG exceeds decoder input buffer\n");
		goto out;
	}
	memset(output.planes[0].addr, 0, output.planes[0].length);
	memcpy(output.planes[0].addr, jpeg_data, jpeg_size);
	output_used[0] = jpeg_size;
	if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
		perror("VIDIOC_SUBSCRIBE_EVENT");
		goto out;
	}
	if (queue_buffer(fd, &output, output_used) ||
	    wait_source_change(fd, opts->timeout_ms) ||
	    get_format(fd, capture.type, &capture_fmt))
		goto out;
	if (capture_fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M &&
	    capture_fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV422M) {
		fprintf(stderr, "decoder selected unsupported capture format %s\n",
			fourcc_name(capture_fmt.fmt.pix_mp.pixelformat,
				    format_name));
		goto out;
	}
	if (capture_fmt.fmt.pix_mp.width != TEST_WIDTH ||
	    capture_fmt.fmt.pix_mp.height != TEST_HEIGHT) {
		fprintf(stderr, "decoder capture dimensions are not 64x64: %ux%u\n",
			capture_fmt.fmt.pix_mp.width,
			capture_fmt.fmt.pix_mp.height);
		goto out;
	}
	if (capture_fmt.fmt.pix_mp.num_planes != 3) {
		fprintf(stderr, "decoder returned %u planes, expected 3\n",
			capture_fmt.fmt.pix_mp.num_planes);
		goto out;
	}
	if (alloc_queue(fd, &capture, &capture_fmt))
		goto out;
	for (i = 0; i < capture.num_planes; i++)
		memset(capture.planes[i].addr, 0xa5,
		       capture.planes[i].length);
	if (queue_buffer(fd, &capture, capture_used) ||
	    stream_on(fd, &capture) || stream_on(fd, &output) ||
	    dequeue_buffer(fd, &capture, opts->timeout_ms, capture_used) ||
	    dequeue_buffer(fd, &output, opts->timeout_ms, output_used))
		goto out;
	if (output_used[0] != jpeg_size) {
		fprintf(stderr, "decoder returned a short input buffer: %zu\n",
			output_used[0]);
		goto out;
	}
	for (i = 0; i < capture.num_planes; i++) {
		const struct v4l2_plane_pix_format *plane_fmt =
			&capture_fmt.fmt.pix_mp.plane_fmt[i];
		size_t plane_width;
		size_t plane_height;
		uint32_t hash;

		if (!capture_used[i]) {
			fprintf(stderr, "decoder plane %u has zero bytesused\n", i);
			goto out;
		}
		if (plane_fmt->sizeimage > capture.planes[i].length) {
			fprintf(stderr, "decoder plane %u format exceeds buffer\n", i);
			goto out;
		}
		decoded_plane_geometry(capture_fmt.fmt.pix_mp.pixelformat,
				       TEST_WIDTH, TEST_HEIGHT, i,
				       &plane_width, &plane_height);
		if (!active_plane_checksum(capture.planes[i].addr,
					   capture_used[i],
					   plane_fmt->bytesperline,
					   plane_width, plane_height, &hash)) {
			fprintf(stderr,
				"decoder plane %u is short, malformed, or constant\n",
				i);
			goto out;
		}
		printf("decoded plane %u: %zu bytes, active checksum %08x\n",
		       i, capture_used[i], hash);
	}
	ret = 0;
out:
	if (fd >= 0) {
		int cleanup_failed = 0;

		cleanup_failed |= release_queue(fd, &capture) != 0;
		cleanup_failed |= release_queue(fd, &output) != 0;
		if (close(fd)) {
			perror("close decoder");
			cleanup_failed = 1;
		}
		if (!ret && cleanup_failed)
			ret = -1;
	}
	return ret;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s --encoder DEVICE --decoder DEVICE [--timeout MS]\n"
		"       %s --selftest\n",
		program, program);
}

static int parse_options(int argc, char **argv, struct options *opts)
{
	static const struct option long_options[] = {
		{ "encoder", required_argument, NULL, 'e' },
		{ "decoder", required_argument, NULL, 'd' },
		{ "timeout", required_argument, NULL, 't' },
		{ "selftest", no_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{},
	};
	int c;

	opterr = 0;
	while ((c = getopt_long(argc, argv, "e:d:t:sh", long_options, NULL)) != -1) {
		switch (c) {
		case 'e':
			opts->encoder = optarg;
			break;
		case 'd':
			opts->decoder = optarg;
			break;
		case 't': {
			char *end;
			long value;

			errno = 0;
			value = strtol(optarg, &end, 10);
			if (errno || *end || value < MIN_TIMEOUT_MS ||
			    value > MAX_TIMEOUT_MS) {
				fprintf(stderr, "invalid timeout '%s'\n", optarg);
				return -1;
			}
			opts->timeout_ms = value;
			break;
		}
		case 's':
			opts->selftest = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(stderr, argv[0]);
			return -1;
		}
	}
	if (optind != argc)
		return -1;
	if (opts->selftest)
		return opts->encoder || opts->decoder ? -1 : 0;
	if (!opts->encoder || !opts->decoder ||
	    !opts->encoder[0] || !opts->decoder[0] ||
	    !strcmp(opts->encoder, opts->decoder)) {
		fprintf(stderr, "distinct encoder and decoder paths are required\n");
		return -1;
	}
	return 0;
}

static int selftest(void)
{
	static const uint8_t baseline[] = {
		0xff, 0xd8,
		0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x40, 0x00, 0x40,
		0x03, 0x01, 0x21, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
		0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11,
		0x03, 0x11, 0x00, 0x3f, 0x00,
		0x11, 0xff, 0x00, 0x22, 0xff, 0xd9,
	};
	static const uint8_t bad_start[] = { 0x00, 0xd8, 0xff, 0xd9 };
	static const uint8_t checksum_sample[] = {
		0xff, 0xd8, 0x00, 0xff, 0xd9,
	};
	static const uint8_t padded_plane[] = { 1, 2, 3, 99, 4, 5, 6 };
	static const uint8_t linear_plane[] = { 1, 2, 3, 4, 5, 6 };
	static const uint8_t constant_plane[] = { 7, 7, 7, 0, 7, 7, 7 };
	uint32_t plane_hash;

	if (!valid_jpeg(baseline, sizeof(baseline), 64, 64) ||
	    valid_jpeg(baseline, sizeof(baseline), 63, 64) ||
	    valid_jpeg(baseline, sizeof(baseline) - 1, 64, 64) ||
	    valid_jpeg(bad_start, sizeof(bad_start), 64, 64) ||
	    checksum(checksum_sample, sizeof(checksum_sample)) != 0x7a8e9c06U ||
	    !active_plane_checksum(padded_plane, sizeof(padded_plane),
				   4, 3, 2, &plane_hash) ||
	    plane_hash != checksum(linear_plane, sizeof(linear_plane)) ||
	    active_plane_checksum(constant_plane, sizeof(constant_plane),
				  4, 3, 2, &plane_hash) ||
	    !path_has_component("/devices/platform/17003000.jpeg-encoder/video0",
				"17003000.jpeg-encoder") ||
	    path_has_component("/devices/platform/x17003000.jpeg-encoder/video0",
			       "17003000.jpeg-encoder")) {
		fprintf(stderr, "JPEG helper test failed\n");
		return -1;
	}
	puts("JPEG helper test passed");
	return 0;
}

int main(int argc, char **argv)
{
	struct options opts = {
		.timeout_ms = DEFAULT_TIMEOUT_MS,
	};
	uint8_t *jpeg_data = NULL;
	size_t jpeg_size = 0;
	int ret = EXIT_FAILURE;

	if (parse_options(argc, argv, &opts)) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
	if (opts.selftest)
		return selftest() ? EXIT_FAILURE : EXIT_SUCCESS;
	if (encode_one(&opts, &jpeg_data, &jpeg_size))
		goto out;
	printf("encoded JPEG: %zu bytes, checksum %08x\n", jpeg_size,
	       (unsigned int)checksum(jpeg_data, jpeg_size));
	if (decode_one(&opts, jpeg_data, jpeg_size))
		goto out;
	puts("MT6797 JPEG encode/decode qualification passed");
	ret = EXIT_SUCCESS;
out:
	free(jpeg_data);
	return ret;
}
