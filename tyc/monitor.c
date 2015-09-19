/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <getopt.h>
#include <unistd.h>
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif
#include "ty.h"
#include "main.h"

enum {
    MONITOR_OPTION_NORESET = 0x200,
    MONITOR_OPTION_TIMEOUT_EOF
};

static const char *short_options = MAIN_SHORT_OPTIONS "b:d:D:f:p:rRs";
static const struct option long_options[] = {
    MAIN_LONG_OPTIONS

    {"baud",        required_argument, NULL, 'b'},
    {"databits",    required_argument, NULL, 'd'},
    {"direction",   required_argument, NULL, 'D'},
    {"flow",        required_argument, NULL, 'f'},
    {"noreset",     no_argument,       NULL, MONITOR_OPTION_NORESET},
    {"parity",      required_argument, NULL, 'p'},
    {"raw",         no_argument,       NULL, 'r'},
    {"reconnect",   no_argument,       NULL, 'R'},
    {"silent",      no_argument,       NULL, 's'},
    {"timeout-eof", required_argument, NULL, MONITOR_OPTION_TIMEOUT_EOF},
    {0}
};

enum {
    DIRECTION_INPUT = 1,
    DIRECTION_OUTPUT = 2
};

#define BUFFER_SIZE 1024
#define ERROR_IO_TIMEOUT 5000

static int terminal_flags = 0;
static uint32_t device_rate = 115200;
static int device_flags = 0;
static int directions = DIRECTION_INPUT | DIRECTION_OUTPUT;
static bool reconnect = false;
static int timeout_eof = 200;

#ifdef _WIN32
static bool fake_echo;

static bool input_run = true;
static HANDLE input_thread;

static HANDLE input_available;
static HANDLE input_processed;

static char input_line[BUFFER_SIZE];
static ssize_t input_ret;
#endif

void print_monitor_usage(FILE *f)
{
    fprintf(f, "usage: tyc monitor [options]\n\n");

    print_main_options(f);
    fprintf(f, "\n");

    fprintf(f, "Monitor options:\n"
               "   -b, --baud <rate>        Use baudrate for serial port\n"
               "   -d, --databits <bits>    Change number of bits for each character\n"
               "                            Must be one of 5, 6, 7 or 8 (default)\n"
               "   -D, --direction <dir>    Open serial connection in given direction\n"
               "                            Supports input, output, both (default)\n"
               "   -f, --flow <control>     Define flow-control mode\n"
               "                            Supports xonxoff (x), rtscts (h) and none (n)\n"

               "   -p, --parity <bits>      Change parity mode to use for the serial port\n"
               "                            Supports odd (o), even (e) and none (n)\n\n"

               "   -r, --raw                Disable line-buffering and line-editing\n"
               "   -s, --silent             Disable echoing of local input on terminal\n\n"

               "   -R, --reconnect          Try to reconnect on I/O errors\n"
               "       --noreset            Don't reset serial port when closing\n"
               "       --timeout-eof <ms>   Time before closing after EOF on standard input\n"
               "                            Defaults to %d ms, use -1 to disable\n", timeout_eof);
}

static int redirect_stdout(int *routfd)
{
    int outfd, r;

    outfd = dup(STDOUT_FILENO);
    if (outfd < 0)
        return ty_error(TY_ERROR_SYSTEM, "dup() failed: %s", strerror(errno));

    r = dup2(STDERR_FILENO, STDOUT_FILENO);
    if (r < 0)
        return ty_error(TY_ERROR_SYSTEM, "dup2() failed: %s", strerror(errno));

    *routfd = outfd;
    return 0;
}

#ifdef _WIN32

static unsigned int __stdcall stdin_thread(void *udata)
{
    TY_UNUSED(udata);

    DWORD len;
    BOOL success;
    int r;

    while (input_run) {
        WaitForSingleObject(input_processed, INFINITE);
        ResetEvent(input_processed);

        success = ReadFile(GetStdHandle(STD_INPUT_HANDLE), input_line, sizeof(input_line), &len, NULL);
        if (!success) {
            r = ty_error(TY_ERROR_IO, "I/O error while reading standard input");
            goto error;
        }
        if (!len) {
            r = 0;
            goto error;
        }

        input_ret = (ssize_t)len;
        SetEvent(input_available);
    }

    return 0;

error:
    input_ret = r;
    SetEvent(input_available);
    return 0;
}

static int start_stdin_thread(void)
{
    input_available = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!input_available)
        return ty_error(TY_ERROR_SYSTEM, "CreateEvent() failed: %s", ty_win32_strerror(0));

    input_processed = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (!input_processed)
        return ty_error(TY_ERROR_SYSTEM, "CreateEvent() failed: %s", ty_win32_strerror(0));

    input_thread = (HANDLE)_beginthreadex(NULL, 0, stdin_thread, NULL, 0, NULL);
    if (!input_thread)
        return ty_error(TY_ERROR_SYSTEM, "_beginthreadex() failed: %s", ty_win32_strerror(0));

    return 0;
}

static void stop_stdin_thread(void)
{
    if (input_thread) {
        CONSOLE_SCREEN_BUFFER_INFO sb;
        INPUT_RECORD ir = {0};
        DWORD written;

        // This is not enough because the background thread may be blocked in ReadFile
        input_run = false;
        SetEvent(input_processed);

        /* We'll soon push VK_RETURN to the console input, which will result in a newline,
           so move the cursor up one line to avoid showing it. */
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &sb);
        if (sb.dwCursorPosition.Y > 0) {
            sb.dwCursorPosition.Y--;
            SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), sb.dwCursorPosition);
        }

        ir.EventType = KEY_EVENT;
        ir.Event.KeyEvent.bKeyDown = TRUE;
        ir.Event.KeyEvent.dwControlKeyState = 0;
        ir.Event.KeyEvent.uChar.AsciiChar = '\r';
        ir.Event.KeyEvent.wRepeatCount = 1;

        // Write a newline to snap the background thread out of the blocking ReadFile call
        WriteConsoleInput(GetStdHandle(STD_INPUT_HANDLE), &ir, 1, &written);

        WaitForSingleObject(input_thread, INFINITE);
        CloseHandle(input_thread);
    }

    if (input_processed)
        CloseHandle(input_processed);
    if (input_available)
        CloseHandle(input_available);
}

#endif

static void fill_descriptor_set(ty_descriptor_set *set, tyb_board *board)
{
    ty_descriptor_set_clear(set);

    tyb_monitor_get_descriptors(tyb_board_get_manager(board), set, 1);
    if (directions & DIRECTION_INPUT)
        tyb_board_get_descriptors(board, TYB_BOARD_CAPABILITY_SERIAL, set, 2);
#ifdef _WIN32
    if (directions & DIRECTION_OUTPUT) {
        if (input_available) {
            ty_descriptor_set_add(set, input_available, 3);
        } else {
            ty_descriptor_set_add(set, GetStdHandle(STD_INPUT_HANDLE), 3);
        }
    }
#else
    if (directions & DIRECTION_OUTPUT)
        ty_descriptor_set_add(set, STDIN_FILENO, 3);
#endif
}

static int loop(tyb_board *board, int outfd)
{
    ty_descriptor_set set = {0};
    int timeout;
    char buf[BUFFER_SIZE];
    ssize_t r;

restart:
    r = tyb_board_serial_set_attributes(board, device_rate, device_flags);
    if (r < 0)
        return (int)r;

    fill_descriptor_set(&set, board);
    timeout = -1;

    printf("Connection ready\n");

    while (true) {
        if (!set.count)
            return 0;

        r = ty_poll(&set, timeout);
        if (r < 0)
            return (int)r;

        switch (r) {
        case 0:
            return 0;

        case 1:
            r = tyb_monitor_refresh(tyb_board_get_manager(board));
            if (r < 0)
                return (int)r;

            if (!tyb_board_has_capability(board, TYB_BOARD_CAPABILITY_SERIAL)) {
                if (!reconnect)
                    return 0;

                printf("Waiting for device...\n");
                r = tyb_board_wait_for(board, TYB_BOARD_CAPABILITY_SERIAL, false, -1);
                if (r < 0)
                    return (int)r;

                goto restart;
            }

            break;

        case 2:
            r = tyb_board_serial_read(board, buf, sizeof(buf), 0);
            if (r < 0) {
                if (r == TY_ERROR_IO && reconnect) {
                    timeout = ERROR_IO_TIMEOUT;
                    ty_descriptor_set_remove(&set, 2);
                    ty_descriptor_set_remove(&set, 3);
                    break;
                }
                return (int)r;
            }

#ifdef _WIN32
            r = write(outfd, buf, (unsigned int)r);
#else
            r = write(outfd, buf, (size_t)r);
#endif
            if (r < 0) {
                if (errno == EIO)
                    return ty_error(TY_ERROR_IO, "I/O error on standard output");
                return ty_error(TY_ERROR_IO, "Failed to write to standard output: %s",
                                strerror(errno));
            }

            break;

        case 3:
#ifdef _WIN32
            if (input_available) {
                if (input_ret < 0)
                    return input_ret;

                memcpy(buf, input_line, (size_t)input_ret);
                r = input_ret;

                ResetEvent(input_available);
                SetEvent(input_processed);
            } else {
                r = read(STDIN_FILENO, buf, sizeof(buf));
            }
#else
            r = read(STDIN_FILENO, buf, sizeof(buf));
#endif
            if (r < 0) {
                if (errno == EIO)
                    return ty_error(TY_ERROR_IO, "I/O error on standard input");
                return ty_error(TY_ERROR_IO, "Failed to read from standard input: %s",
                                strerror(errno));
            }
            if (!r) {
                if (timeout_eof >= 0) {
                    /* EOF reached, don't listen to stdin anymore, and start timeout to give some
                       time for the device to send any data before closing down. */
                    timeout = timeout_eof;
                    ty_descriptor_set_remove(&set, 1);
                    ty_descriptor_set_remove(&set, 3);
                }
                break;
            }

#ifdef _WIN32
            if (fake_echo) {
                r = write(outfd, buf, (unsigned int)r);
                if (r < 0)
                    return (int)r;
            }
#endif

            r = tyb_board_serial_write(board, buf, (size_t)r);
            if (r < 0) {
                if (r == TY_ERROR_IO && reconnect) {
                    timeout = ERROR_IO_TIMEOUT;
                    ty_descriptor_set_remove(&set, 2);
                    ty_descriptor_set_remove(&set, 3);
                    break;
                }
                return (int)r;
            }

            break;
        }
    }
}

int monitor(int argc, char *argv[])
{
    tyb_board *board = NULL;
    int outfd = -1;
    int r;

    int c;
    while ((c = getopt_long(argc, argv, short_options, long_options, NULL)) != -1) {
        switch (c) {
        case 's':
            terminal_flags |= TY_TERMINAL_SILENT;
            break;
        case 'r':
            terminal_flags |= TY_TERMINAL_RAW;
            break;

        case 'D':
            if (strcmp(optarg, "input") == 0) {
                directions = DIRECTION_INPUT;
            } else if (strcmp(optarg, "output") == 0) {
                directions = DIRECTION_OUTPUT;
            } else if (strcmp(optarg, "both") == 0) {
                directions = DIRECTION_INPUT | DIRECTION_OUTPUT;
            } else {
                return ty_error(TY_ERROR_PARAM, "--direction must be one off input, output or both");
            }
            break;

        case 'b':
            errno = 0;
            device_rate = (uint32_t)strtoul(optarg, NULL, 10);
            if (errno)
                return ty_error(TY_ERROR_PARAM, "--baud requires a number");
            break;
        case 'd':
           device_flags &= ~TYD_SERIAL_CSIZE_MASK;
            if (strcmp(optarg, "5") == 0) {
                device_flags |= TYD_SERIAL_5BITS_CSIZE;
            } else if (strcmp(optarg, "6") == 0) {
                device_flags |= TYD_SERIAL_6BITS_CSIZE;
            } else if (strcmp(optarg, "7") == 0) {
                device_flags |= TYD_SERIAL_7BITS_CSIZE;
            } else if (strcmp(optarg, "8") != 0) {
                return ty_error(TY_ERROR_PARAM, "--databits must be one off 5, 6, 7 or 8");
            }
        case 'f':
            device_flags &= ~TYD_SERIAL_FLOW_MASK;
            if (strcmp(optarg, "x") == 0 || strcmp(optarg, "xonxoff") == 0) {
                device_flags |= TYD_SERIAL_XONXOFF_FLOW;
            } else if (strcmp(optarg, "h") == 0 || strcmp(optarg, "rtscts") == 0) {
                device_flags |= TYD_SERIAL_RTSCTS_FLOW;
            } else if (strcmp(optarg, "n") != 0 && strcmp(optarg, "none") == 0) {
                return ty_error(TY_ERROR_PARAM,
                                "--flow must be one off x (xonxoff), h (rtscts) or n (none)");
            }
            break;
        case MONITOR_OPTION_NORESET:
            device_flags |= TYD_SERIAL_NOHUP_CLOSE;
            break;
        case 'p':
            device_flags &= ~TYD_SERIAL_PARITY_MASK;
            if (strcmp(optarg, "o") == 0 || strcmp(optarg, "odd") == 0) {
                device_flags |= TYD_SERIAL_ODD_PARITY;
            } else if (strcmp(optarg, "e") == 0 || strcmp(optarg, "even") == 0) {
                device_flags |= TYD_SERIAL_EVEN_PARITY;
            } else if (strcmp(optarg, "n") != 0 && strcmp(optarg, "none") != 0) {
                return ty_error(TY_ERROR_PARAM,
                                "--parity must be one off o (odd), e (even) or n (none)");
            }
            break;

        case 'R':
            reconnect = true;
            break;

        case MONITOR_OPTION_TIMEOUT_EOF:
            errno = 0;
            timeout_eof = (int)strtol(optarg, NULL, 10);
            if (errno)
                return ty_error(TY_ERROR_PARSE, "--timeout requires a number");
            if (timeout_eof < 0)
                timeout_eof = -1;
            break;

        default:
            r = parse_main_option(argc, argv, c);
            if (r <= 0)
                return r;
            break;
        }
    }

    if (argc > optind) {
        ty_error(TY_ERROR_PARAM, "No positional argument is allowed");
        goto usage;
    }

    if (ty_terminal_available(TY_DESCRIPTOR_STDIN)) {
#ifdef _WIN32
        if (terminal_flags & TY_TERMINAL_RAW && !(terminal_flags & TY_TERMINAL_SILENT)) {
            terminal_flags |= TY_TERMINAL_SILENT;

            if (ty_terminal_available(TY_DESCRIPTOR_STDOUT))
                fake_echo = true;
        }

        /* Unlike POSIX platforms, Windows does not implement the console line editing behavior
         * at the tty layer. Instead, ReadFile() takes care of it and blocks until return is hit.
         * The problem is that the Wait functions will return the stdin descriptor as soon as
         * something is typed but then, ReadFile() will block until return is pressed.
         * Overlapped I/O cannot be used because it is not supported on console descriptors.
         *
         * So the best way I found is to have a background thread handle the blocking ReadFile()
         * and pass the lines in a buffer. When a new line is entered, the input_available
         * event is set to signal the poll in loop(). I also tried to use an anonymous pipe to
         * make it simpler, but the Wait functions do not support them. */
        if (directions & DIRECTION_OUTPUT && !(terminal_flags & TY_TERMINAL_RAW)) {
            r = start_stdin_thread();
            if (r < 0)
                goto cleanup;
        }
#endif

        r = ty_terminal_setup(terminal_flags);
        if (r < 0)
            goto cleanup;
    }

    r = redirect_stdout(&outfd);
    if (r < 0)
        goto cleanup;

    r = get_board(&board);
    if (r < 0)
        goto cleanup;

    r = loop(board, outfd);

cleanup:
#ifdef _WIN32
    stop_stdin_thread();
#endif
    tyb_board_unref(board);
    return r;

usage:
    print_monitor_usage(stderr);
    return TY_ERROR_PARAM;
}