// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_DEVICE "/dev/video30"
#define OUTPUT_DEVICE "/dev/video35"
#define OUTPUT_CAPTURE_ACTIVE "/sys/class/video4linux/video35/capture_active"
#define CAPTURE_ACTIVE_POLL_MS 250
#define FRAME_WIDTH 1920U
#define FRAME_HEIGHT 1080U
#define BUFFER_COUNT 4U

struct mapped_buffer {
    void *address;
    size_t length;
};

static volatile sig_atomic_t running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

/* How many openers have a capture streamed on the public node, from the
 * attribute patches/v4l2loopback/0001-* adds. The public producer remains
 * registered so browsers can discover the camera, but the private hardware
 * node is opened only while this count is non-zero.
 *
 * A read that fails is treated as "something is capturing": on a kernel whose
 * loopback lacks the attribute this degrades to the old unconditional copy
 * rather than to a camera that silently publishes nothing.
 */
static bool capture_is_active(void)
{
    char text[32];
    ssize_t got;
    int fd = open(OUTPUT_CAPTURE_ACTIVE, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return true;

    got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0)
        return true;

    text[got] = '\0';
    return strtol(text, NULL, 10) > 0;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR && running);

    return result;
}

static void fail(const char *operation)
{
    fprintf(stderr, "chiron-camera-bridge: %s: %s\n", operation,
            strerror(errno));
}

/* Amlogic's userspace ISP context does not survive a complete V4L2
 * STREAMOFF/close cycle. Recreate that context before the next capture while
 * keeping the public loopback producer alive for the browser.
 */
static int control_isp_context(const char *action)
{
    pid_t pid = fork();
    pid_t waited;
    int status;

    if (pid < 0) {
        fail("forking ISP restart helper");
        return -1;
    }
    if (pid == 0) {
        execl("/usr/bin/systemctl", "systemctl", action,
              "chiron-camera.service", (char *)NULL);
        _exit(127);
    }

    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR && running);
    if (waited < 0) {
        fail("waiting for ISP restart helper");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "chiron-camera-bridge: ISP %s failed\n", action);
        errno = EIO;
        return -1;
    }

    /* chiron-camera.service is Type=simple; ispService still needs time to
     * configure the media graph after systemd has started its process.
     */
    if (poll(NULL, 0, 3000) < 0 && errno != EINTR) {
        fail("waiting for ISP context");
        return -1;
    }
    return running ? 0 : -1;
}

static int restart_isp_context(void)
{
    return control_isp_context("restart");
}

static int stop_isp_context(void)
{
    return control_isp_context("stop");
}

static int set_format(int fd, enum v4l2_buf_type type, const char *name)
{
    struct v4l2_format format;

    memset(&format, 0, sizeof(format));
    format.type = type;
    format.fmt.pix.width = FRAME_WIDTH;
    format.fmt.pix.height = FRAME_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        fail(name);
        return -1;
    }

    if (format.fmt.pix.width != FRAME_WIDTH ||
        format.fmt.pix.height != FRAME_HEIGHT ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
        fprintf(stderr,
                "chiron-camera-bridge: %s returned unsupported format "
                "%ux%u %.4s\n",
                name, format.fmt.pix.width, format.fmt.pix.height,
                (const char *)&format.fmt.pix.pixelformat);
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int set_capture_rate(int fd)
{
    struct v4l2_streamparm parameters;

    memset(&parameters, 0, sizeof(parameters));
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = 30;

    if (xioctl(fd, VIDIOC_S_PARM, &parameters) < 0 && errno != EINVAL) {
        fail("setting capture rate");
        return -1;
    }

    return 0;
}

static int write_frame(int fd, const void *data, size_t length)
{
    while (running) {
        ssize_t written = write(fd, data, length);

        if (written == (ssize_t)length)
            return 0;
        if (written >= 0) {
            fprintf(stderr,
                    "chiron-camera-bridge: short frame write: %zd of %zu\n",
                    written, length);
            errno = EIO;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd descriptor = {
                .fd = fd,
                .events = POLLOUT,
            };

            if (poll(&descriptor, 1, 1000) >= 0)
                continue;
            if (errno == EINTR)
                continue;
        }

        fail("writing public camera frame");
        return -1;
    }

    return 0;
}

struct capture_session {
    struct mapped_buffer buffers[BUFFER_COUNT];
    unsigned int mapped_count;
    bool streaming;
    int fd;
};

static void stop_capture(struct capture_session *session)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (session->streaming && xioctl(session->fd, VIDIOC_STREAMOFF, &type) < 0)
        fail("stopping hardware capture");

    for (unsigned int index = 0; index < session->mapped_count; ++index) {
        if (session->buffers[index].address != NULL)
            munmap(session->buffers[index].address,
                   session->buffers[index].length);
    }

    if (session->fd >= 0)
        close(session->fd);

    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

static int start_capture(struct capture_session *session)
{
    struct v4l2_requestbuffers request;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    session->fd = open(CAPTURE_DEVICE, O_RDWR | O_CLOEXEC);
    if (session->fd < 0) {
        fail("opening " CAPTURE_DEVICE);
        return -1;
    }

    if (set_format(session->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                   "setting hardware capture format") < 0 ||
        set_capture_rate(session->fd) < 0)
        goto failure;

    memset(&request, 0, sizeof(request));
    request.count = BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(session->fd, VIDIOC_REQBUFS, &request) < 0) {
        fail("requesting hardware capture buffers");
        goto failure;
    }
    if (request.count != BUFFER_COUNT) {
        fprintf(stderr,
                "chiron-camera-bridge: hardware allocated %u buffers, "
                "expected %u\n",
                request.count, BUFFER_COUNT);
        errno = ENOMEM;
        goto failure;
    }

    for (unsigned int index = 0; index < BUFFER_COUNT; ++index) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (xioctl(session->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            fail("querying hardware capture buffer");
            goto failure;
        }

        session->buffers[index].length = buffer.length;
        session->buffers[index].address =
            mmap(NULL, buffer.length, PROT_READ, MAP_SHARED, session->fd,
                 buffer.m.offset);
        if (session->buffers[index].address == MAP_FAILED) {
            session->buffers[index].address = NULL;
            fail("mapping hardware capture buffer");
            goto failure;
        }
        ++session->mapped_count;

        if (xioctl(session->fd, VIDIOC_QBUF, &buffer) < 0) {
            fail("queueing hardware capture buffer");
            goto failure;
        }
    }

    if (xioctl(session->fd, VIDIOC_STREAMON, &type) < 0) {
        fail("starting hardware capture");
        goto failure;
    }
    session->streaming = true;
    return 0;

failure:
    stop_capture(session);
    return -1;
}

static int copy_frame(struct capture_session *session, int output_fd)
{
    struct v4l2_buffer buffer;

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (xioctl(session->fd, VIDIOC_DQBUF, &buffer) < 0) {
        if (!running && errno == EINTR)
            return 0;
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        fail("dequeueing hardware capture buffer");
        return -1;
    }
    if (buffer.index >= session->mapped_count ||
        buffer.bytesused > session->buffers[buffer.index].length) {
        fprintf(stderr,
                "chiron-camera-bridge: invalid capture buffer %u/%u, "
                "%u bytes\n",
                buffer.index, session->mapped_count, buffer.bytesused);
        errno = EIO;
        return -1;
    }

    if (write_frame(output_fd, session->buffers[buffer.index].address,
                    buffer.bytesused) < 0)
        return -1;

    if (xioctl(session->fd, VIDIOC_QBUF, &buffer) < 0) {
        fail("requeueing hardware capture buffer");
        return -1;
    }

    return 0;
}

int main(void)
{
    struct capture_session capture;
    enum v4l2_buf_type output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    struct sigaction action;
    int output_fd = -1;
    int status = EXIT_FAILURE;
    bool isp_restart_required = true;

    memset(&capture, 0, sizeof(capture));
    capture.fd = -1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0) {
        fail("installing signal handlers");
        goto cleanup;
    }

    output_fd = open(OUTPUT_DEVICE, O_RDWR | O_CLOEXEC);
    if (output_fd < 0) {
        fail("opening " OUTPUT_DEVICE);
        goto cleanup;
    }

    if (set_format(output_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                   "setting public output format") < 0)
        goto cleanup;

    /* Declare the producer before any frame is written.
     *
     * v4l2loopback only advertises VIDEO_CAPTURE once a writer has arrived,
     * and with exclusive_caps=1 a node with no writer offers OUTPUT alone --
     * a client trying to capture from it gets "unsupported stream type" and
     * VIDIOC_G_FMT fails. It used to arrive implicitly, on the first write().
     *
     * That stopped being enough once the loop learned to skip the copy while
     * nothing is capturing: with no writes there was no writer, with no writer
     * there was no capture capability, and with no capture there was never
     * anything to make the loop start writing. Streaming the output on here,
     * once, breaks that circle -- it marks the node ready for the whole
     * lifetime of the process, whether or not a frame follows.
     */
    if (xioctl(output_fd, VIDIOC_STREAMON, &output_type) < 0) {
        fail("declaring the bridge as the public node's producer");
        goto cleanup;
    }

    fprintf(stderr,
            "chiron-camera-bridge: public camera ready; hardware is off "
            "until a client captures\n");

    /* Enforce a genuinely idle boot when upgrading from an image where the
     * ISP service was still enabled directly. Closing only the V4L2 output
     * node leaves the vendor sensor/adapter pipeline generating interrupts.
     */
    if (stop_isp_context() < 0)
        goto cleanup;

    while (running) {
        if (!capture_is_active()) {
            if (capture.streaming) {
                stop_capture(&capture);
                if (stop_isp_context() < 0)
                    goto cleanup;
                isp_restart_required = true;
                fprintf(stderr,
                        "chiron-camera-bridge: no client is capturing; "
                        "hardware closed\n");
            }
            if (poll(NULL, 0, CAPTURE_ACTIVE_POLL_MS) < 0 &&
                errno != EINTR) {
                fail("waiting for a camera client");
                goto cleanup;
            }
            continue;
        }

        if (!capture.streaming) {
            if (isp_restart_required) {
                fprintf(stderr,
                        "chiron-camera-bridge: rebuilding ISP context\n");
                if (restart_isp_context() < 0)
                    goto cleanup;
                isp_restart_required = false;
            }
            if (start_capture(&capture) < 0)
                goto cleanup;
            fprintf(stderr,
                    "chiron-camera-bridge: client is capturing; "
                    "hardware started\n");
        }

        if (copy_frame(&capture, output_fd) < 0)
            goto cleanup;
    }

    status = EXIT_SUCCESS;

cleanup:
    stop_capture(&capture);
    if (output_fd >= 0)
        close(output_fd);
    return status;
}
