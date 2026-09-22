// SPDX-License-Identifier: 0BSD
// Copyright (c) 2026 angellfear
/*
 * chiron-bt-init - load the Amlogic W2 Bluetooth firmware over H4 UART.
 *
 * The Chiron Android image does not use w2_bt.ko for its Bluetooth HCI path.
 * libbt-vendor_aml.so powers the controller through rfkill, opens /dev/ttyS1,
 * changes the ROM UART from 115200 to 4000000 baud, downloads ICCM/DCCM with
 * Amlogic vendor HCI commands, starts the controller and sets its address.
 * This program reproduces only that hardware-initialisation part.  Once it
 * exits, BlueZ's btattach can bind the standard kernel H4 line discipline.
 *
 * Protocol and sequencing were checked against the factory Chiron binary and
 * the matching Khadas/Amlogic vendor implementation.  Do not replace this with
 * the unrelated /dev/stpbt or /dev/vhci experiments: neither is the transport
 * used by the factory firmware on this board.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif
#ifndef N_TTY
#define N_TTY 0
#endif
#ifndef N_HCI
#define N_HCI 15
#endif

/* Stable Linux HCI UART uAPI, also used by BlueZ btattach. */
#ifndef HCIUARTSETPROTO
#define HCIUARTSETPROTO _IOW('U', 200, int)
#endif
#ifndef HCIUARTGETDEVICE
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#endif
#ifndef HCIUARTSETFLAGS
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#endif

#define HCI_UART_H4 0
#define HCI_UART_RESET_ON_INIT 1

static volatile sig_atomic_t stop_requested;

static void handle_stop(int signum)
{
    (void)signum;
    stop_requested = 1;
}

#define DEFAULT_UART "/dev/ttyS1"
#define DEFAULT_FIRMWARE "/usr/lib/firmware/amlbt/w2_bt_fw_uart.bin"
#define DEFAULT_WIFI_MAC "/sys/class/net/wlan0/address"

#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT 0x04
#define EVT_CMD_COMPLETE 0x0e
#define EVT_CMD_STATUS 0x0f

#define TCI_READ_REG 0xfef0
#define TCI_WRITE_REG 0xfef1
#define TCI_UPDATE_UART_BAUDRATE 0xfef2
#define TCI_DOWNLOAD_BT_FW 0xfef3
#define HCI_VSC_WRITE_BD_ADDR 0xfc1a
#define HCI_VSC_SET_SLEEP_MODE 0xfc27

#define REG_DOWNLOAD_EVENT 0x00a70014
#define REG_START_GATING 0x00a7000c
#define REG_DEV_RESET 0x00f03058
#define REG_PMU_POWER_CFG 0x00f03040
#define REG_RAM_PD_SHUTDOWN_SW 0x00f03050

#define ICCM_RAM_BASE 0x00000000
#define DCCM_RAM_BASE 0x00d00000
#define ICCM_ROM_SIZE (256U * 1024U)
#define DOWNLOAD_CHUNK 248U
#define COMMAND_TIMEOUT_MS 3000

static const char *progname;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: %s [-A] [-d UART] [-f FIRMWARE] [-a auto|BDADDR] [-v]\n"
            "  default UART:     %s\n"
            "  default firmware: %s\n"
            "  default address:  Wi-Fi MAC + 1\n"
            "  -A: attach N_HCI/H4 and hold the UART until SIGTERM\n",
            progname, DEFAULT_UART, DEFAULT_FIRMWARE);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = value & 0xff;
    p[1] = (value >> 8) & 0xff;
    p[2] = (value >> 16) & 0xff;
    p[3] = (value >> 24) & 0xff;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_deadline(int fd, void *buf, size_t len, long long deadline)
{
    uint8_t *p = buf;

    while (len) {
        long long left = deadline - monotonic_ms();
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc;

        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        rc = poll(&pfd, 1, left > 1000 ? 1000 : (int)left);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int wait_command_event(int fd, uint16_t wanted_opcode, bool verbose)
{
    long long deadline = monotonic_ms() + COMMAND_TIMEOUT_MS;

    for (;;) {
        uint8_t type, hdr[2], payload[255];

        if (read_deadline(fd, &type, 1, deadline) < 0)
            return -1;
        if (type != HCI_EVENT_PKT) {
            if (verbose)
                fprintf(stderr, "%s: ignoring H4 packet type 0x%02x\n",
                        progname, type);
            continue;
        }
        if (read_deadline(fd, hdr, sizeof(hdr), deadline) < 0 ||
            read_deadline(fd, payload, hdr[1], deadline) < 0)
            return -1;

        if (hdr[0] == EVT_CMD_COMPLETE && hdr[1] >= 4) {
            uint16_t opcode = payload[1] | ((uint16_t)payload[2] << 8);
            if (opcode != wanted_opcode) {
                if (verbose)
                    fprintf(stderr,
                            "%s: event for opcode 0x%04x while waiting for 0x%04x\n",
                            progname, opcode, wanted_opcode);
                continue;
            }
            if (payload[3] != 0) {
                fprintf(stderr, "%s: opcode 0x%04x failed, status 0x%02x\n",
                        progname, opcode, payload[3]);
                errno = EPROTO;
                return -1;
            }
            return 0;
        }
        if (hdr[0] == EVT_CMD_STATUS && hdr[1] >= 4) {
            uint16_t opcode = payload[2] | ((uint16_t)payload[3] << 8);
            if (opcode != wanted_opcode)
                continue;
            if (payload[0] != 0) {
                fprintf(stderr, "%s: opcode 0x%04x status 0x%02x\n",
                        progname, opcode, payload[0]);
                errno = EPROTO;
                return -1;
            }
            return 0;
        }
        if (verbose)
            fprintf(stderr, "%s: ignoring event 0x%02x (%u bytes)\n",
                    progname, hdr[0], hdr[1]);
    }
}

static int hci_command(int fd, uint16_t opcode, const void *params,
                       uint8_t params_len, bool verbose)
{
    uint8_t packet[4 + 255];

    packet[0] = HCI_COMMAND_PKT;
    packet[1] = opcode & 0xff;
    packet[2] = opcode >> 8;
    packet[3] = params_len;
    if (params_len)
        memcpy(packet + 4, params, params_len);
    if (write_all(fd, packet, (size_t)params_len + 4) < 0)
        return -1;
    return wait_command_event(fd, opcode, verbose);
}

static int set_uart_speed(int fd, speed_t speed)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) < 0)
        return -1;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0)
        return -1;
    return tcsetattr(fd, TCSANOW, &tio);
}

static int attach_hci_uart(int fd)
{
    struct sigaction action;
    struct pollfd pfd = { .fd = fd, .events = 0 };
    unsigned long flags = 1UL << HCI_UART_RESET_ON_INIT;
    int ldisc = N_HCI;
    int dev_id;
    int rc = -1;
    int saved;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0)
        return -1;

    if (tcflush(fd, TCIOFLUSH) < 0)
        return -1;
    if (ioctl(fd, TIOCSETD, &ldisc) < 0)
        return -1;
    if (ioctl(fd, HCIUARTSETFLAGS, flags) < 0)
        goto restore;
    if (ioctl(fd, HCIUARTSETPROTO, HCI_UART_H4) < 0)
        goto restore;

    dev_id = ioctl(fd, HCIUARTGETDEVICE);
    if (dev_id < 0)
        goto restore;

    fprintf(stderr,
            "%s: attached /dev hci%d with N_HCI/H4, reset-on-init; holding UART\n",
            progname, dev_id);

    while (!stop_requested) {
        int poll_rc = poll(&pfd, 1, -1);

        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            goto restore;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            goto restore;
        }
    }

    rc = 0;

restore:
    saved = errno;
    ldisc = N_TTY;
    if (ioctl(fd, TIOCSETD, &ldisc) < 0 && rc == 0) {
        saved = errno;
        rc = -1;
    }
    errno = saved;
    return rc;
}

static int register_write(int fd, uint32_t address, uint32_t value,
                          bool verbose)
{
    uint8_t params[8];

    put_le32(params, address);
    put_le32(params + 4, value);
    return hci_command(fd, TCI_WRITE_REG, params, sizeof(params), verbose);
}

static int register_read(int fd, uint32_t address, bool verbose)
{
    uint8_t params[4];

    put_le32(params, address);
    return hci_command(fd, TCI_READ_REG, params, sizeof(params), verbose);
}

static int load_file(const char *path, uint8_t **data_out, size_t *size_out)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *data;
    long size;

    if (!fp)
        return -1;
    if (fseek(fp, 0, SEEK_END) < 0 || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) < 0) {
        fclose(fp);
        return -1;
    }
    data = malloc((size_t)size);
    if (!data) {
        fclose(fp);
        errno = ENOMEM;
        return -1;
    }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        int saved = ferror(fp) ? errno : EIO;
        free(data);
        fclose(fp);
        errno = saved;
        return -1;
    }
    fclose(fp);
    *data_out = data;
    *size_out = (size_t)size;
    return 0;
}

static int download_region(int fd, const char *name, uint32_t base,
                           const uint8_t *data, size_t offset, size_t length,
                           bool verbose)
{
    size_t done = 0;
    unsigned commands = 0;

    while (done < length) {
        uint8_t params[4 + DOWNLOAD_CHUNK];
        size_t chunk = length - done;

        if (chunk > DOWNLOAD_CHUNK)
            chunk = DOWNLOAD_CHUNK;
        put_le32(params, base + (uint32_t)offset + (uint32_t)done);
        memcpy(params + 4, data + offset + done, chunk);
        if (hci_command(fd, TCI_DOWNLOAD_BT_FW, params,
                        (uint8_t)(chunk + 4), verbose) < 0)
            return -1;
        done += chunk;
        commands++;
        if (verbose && (commands % 128 == 0 || done == length))
            fprintf(stderr, "%s: %s %zu/%zu bytes\n",
                    progname, name, done, length);
    }
    fprintf(stderr, "%s: downloaded %s: %zu bytes in %u commands\n",
            progname, name, length, commands);
    return 0;
}

static int parse_bdaddr(const char *text, uint8_t addr[6])
{
    unsigned v[6];

    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++)
        addr[i] = (uint8_t)v[i];
    return 0;
}

static int automatic_bdaddr(uint8_t addr[6])
{
    FILE *fp = fopen(DEFAULT_WIFI_MAC, "r");
    char text[64];

    if (!fp)
        return -1;
    if (!fgets(text, sizeof(text), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (parse_bdaddr(text, addr) < 0) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 5; i >= 0; i--) {
        addr[i]++;
        if (addr[i] != 0)
            break;
    }
    return 0;
}

static int set_bdaddr(int fd, const uint8_t addr[6], bool verbose)
{
    uint8_t reversed[6];

    for (int i = 0; i < 6; i++)
        reversed[i] = addr[5 - i];
    fprintf(stderr, "%s: Bluetooth address %02x:%02x:%02x:%02x:%02x:%02x\n",
            progname, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return hci_command(fd, HCI_VSC_WRITE_BD_ADDR, reversed,
                       sizeof(reversed), verbose);
}

int main(int argc, char **argv)
{
    static const uint8_t w2_sleep_disabled[12] = { 0 };
    const char *uart = DEFAULT_UART;
    const char *firmware = DEFAULT_FIRMWARE;
    const char *address_arg = "auto";
    uint8_t *image = NULL;
    size_t image_size = 0;
    uint32_t iccm_size, dccm_size;
    uint8_t address[6], params[8];
    bool verbose = false;
    bool attach = false;
    int fd = -1, opt, rc = EXIT_FAILURE;

    progname = argv[0];
    while ((opt = getopt(argc, argv, "Ad:f:a:vh")) != -1) {
        switch (opt) {
        case 'A': attach = true; break;
        case 'd': uart = optarg; break;
        case 'f': firmware = optarg; break;
        case 'a': address_arg = optarg; break;
        case 'v': verbose = true; break;
        case 'h': usage(stdout); return EXIT_SUCCESS;
        default: usage(stderr); return EXIT_FAILURE;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (load_file(firmware, &image, &image_size) < 0) {
        fprintf(stderr, "%s: cannot read %s: %s\n",
                progname, firmware, strerror(errno));
        goto out;
    }
    if (image_size < 8) {
        fprintf(stderr, "%s: firmware header is truncated\n", progname);
        goto out;
    }
    iccm_size = get_le32(image);
    dccm_size = get_le32(image + 4);
    if (iccm_size < ICCM_ROM_SIZE ||
        image_size != 8ULL + iccm_size + dccm_size) {
        fprintf(stderr,
                "%s: invalid firmware layout: file=%zu ICCM=%u DCCM=%u\n",
                progname, image_size, iccm_size, dccm_size);
        goto out;
    }
    fprintf(stderr, "%s: firmware ICCM=%u DCCM=%u\n",
            progname, iccm_size, dccm_size);

    if (!strcmp(address_arg, "auto")) {
        if (automatic_bdaddr(address) < 0) {
            fprintf(stderr, "%s: cannot derive address from %s: %s\n",
                    progname, DEFAULT_WIFI_MAC, strerror(errno));
            goto out;
        }
    } else if (parse_bdaddr(address_arg, address) < 0) {
        fprintf(stderr, "%s: invalid Bluetooth address: %s\n",
                progname, address_arg);
        goto out;
    }

    fd = open(uart, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open %s: %s\n",
                progname, uart, strerror(errno));
        goto out;
    }
    if (set_uart_speed(fd, B115200) < 0 || tcflush(fd, TCIOFLUSH) < 0) {
        fprintf(stderr, "%s: cannot configure %s at 115200: %s\n",
                progname, uart, strerror(errno));
        goto out;
    }

    put_le32(params, 0x00a30128);
    put_le32(params + 4, 0x00007009); /* Amlogic ARM UART divisor for 4 Mbit/s */
    if (hci_command(fd, TCI_UPDATE_UART_BAUDRATE, params,
                    sizeof(params), verbose) < 0) {
        fprintf(stderr, "%s: controller baud-rate switch failed: %s\n",
                progname, strerror(errno));
        goto out;
    }
    if (set_uart_speed(fd, B4000000) < 0) {
        fprintf(stderr, "%s: host 4 Mbit/s setup failed: %s\n",
                progname, strerror(errno));
        goto out;
    }
    fprintf(stderr, "%s: UART switched to 4000000 baud, raw 8N1, no flow control\n",
            progname);

    if (register_write(fd, REG_DOWNLOAD_EVENT, 0x01000000, verbose) < 0 ||
        register_write(fd, REG_RAM_PD_SHUTDOWN_SW, 0, verbose) < 0 ||
        register_read(fd, REG_RAM_PD_SHUTDOWN_SW, verbose) < 0) {
        fprintf(stderr, "%s: controller download setup failed: %s\n",
                progname, strerror(errno));
        goto out;
    }

    if (download_region(fd, "ICCM", ICCM_RAM_BASE, image + 8,
                        ICCM_ROM_SIZE, iccm_size - ICCM_ROM_SIZE,
                        verbose) < 0 ||
        download_region(fd, "DCCM", DCCM_RAM_BASE, image + 8 + iccm_size,
                        0, dccm_size, verbose) < 0) {
        fprintf(stderr, "%s: firmware download failed: %s\n",
                progname, strerror(errno));
        goto out;
    }

    if (register_write(fd, REG_DOWNLOAD_EVENT, 0, verbose) < 0 ||
        register_write(fd, REG_PMU_POWER_CFG, 0x20000000, verbose) < 0 ||
        register_write(fd, REG_START_GATING, 0x08000000, verbose) < 0) {
        fprintf(stderr, "%s: controller start failed: %s\n",
                progname, strerror(errno));
        goto out;
    }

    /*
     * This final write deliberately uses 0xfef2, not the generic register
     * opcode 0xfef1. Factory libbt-vendor_aml.so sends the special CPU-start
     * command here; with 0xfef1 the controller can answer setup commands but
     * later fails classic HCI operations such as inquiry and authentication.
     */
    put_le32(params, REG_DEV_RESET);
    put_le32(params + 4, 0x00070000);
    if (hci_command(fd, TCI_UPDATE_UART_BAUDRATE, params,
                    sizeof(params), verbose) < 0) {
        fprintf(stderr, "%s: controller CPU start failed: %s\n",
                progname, strerror(errno));
        goto out;
    }
    if (set_bdaddr(fd, address, verbose) < 0) {
        fprintf(stderr, "%s: setting Bluetooth address failed: %s\n",
                progname, strerror(errno));
        goto out;
    }

    /* Linux has no Android UPIO wake hook, so leave W2 sleep disabled. */
    usleep(100000);
    if (hci_command(fd, HCI_VSC_SET_SLEEP_MODE, w2_sleep_disabled,
                    sizeof(w2_sleep_disabled), verbose) < 0) {
        fprintf(stderr, "%s: disabling controller sleep failed: %s\n",
                progname, strerror(errno));
        goto out;
    }
    usleep(100000);

    if (attach) {
        free(image);
        image = NULL;
        if (close(fd) < 0) {
            fprintf(stderr, "%s: closing initialized UART failed: %s\n",
                    progname, strerror(errno));
            fd = -1;
            goto out;
        }
        fd = open(uart, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (fd < 0 || set_uart_speed(fd, B4000000) < 0) {
            fprintf(stderr, "%s: reopening %s at 4 Mbit/s failed: %s\n",
                    progname, uart, strerror(errno));
            goto out;
        }
        fprintf(stderr,
                "%s: reopened %s at 4000000 baud for kernel HCI hand-off\n",
                progname, uart);
        if (attach_hci_uart(fd) < 0) {
            fprintf(stderr, "%s: HCI UART attach failed: %s\n",
                    progname, strerror(errno));
            goto out;
        }
        fprintf(stderr, "%s: HCI UART detached cleanly\n", progname);
    } else {
        fprintf(stderr, "%s: W2 controller is ready for btattach\n", progname);
    }
    rc = EXIT_SUCCESS;

out:
    if (fd >= 0)
        close(fd);
    free(image);
    return rc;
}
