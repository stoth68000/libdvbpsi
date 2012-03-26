/*****************************************************************************
 * dvbinfo.c: DVB PSI Information
 *****************************************************************************
 * Copyright (C) 2010-2011 M2X BV
 *
 * Authors: Jean-Paul Saman <jpsaman@videolan.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *****************************************************************************/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>

#if defined(HAVE_INTTYPES_H)
#   include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#   include <stdint.h>
#endif

#include <pthread.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <syslog.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_SOCKET_H
#   include <netdb.h>
#   include <sys/socket.h>
#   include <netinet/in.h>
#   include <arpa/inet.h>
#endif

#include <assert.h>

#include "dvbinfo.h"
#include "libdvbpsi.h"
#include "buffer.h"

#ifdef HAVE_SYS_SOCKET_H
#   include "udp.h"
#   include "tcp.h"
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static const int   i_summary_mode[] = { SUM_BANDWIDTH, SUM_TABLE, SUM_PACKET, SUM_WIRE };
static const char *psz_summary_mode[] = { "bandwidth", "table", "packet", "wire" };

/*****************************************************************************
 *
 *****************************************************************************/
typedef struct dvbinfo_capture_s
{
    fifo_t  *fifo;
    fifo_t  *empty;

    size_t   size;  /* prefered capture size */

    params_t *params;
    bool      b_alive;
} dvbinfo_capture_t;

/*****************************************************************************
 * Usage
 *****************************************************************************/
static void usage(void)
{
#ifdef HAVE_SYS_SOCKET_H
    printf("Usage: dvbinfo [-h] [-d <debug>] [-f|-m| [[-u|-t] -i <ipaddress:port>] -o <outputfile>\n");
    printf("               [-s [bandwidth|table|packet] --summary-file <file> --summary-period <ms>]\n");
#else
    printf("Usage: dvbinfo [-h] [-d <debug>] [-f|\n");
#endif
    printf("\n");
    printf(" -d | --debug          : debug level (default:none, error, warn, debug)\n");
    printf(" -h | --help           : help information\n");
    printf("\nInputs: \n");
    printf(" -f | --file           : filename\n");
#ifdef HAVE_SYS_SOCKET_H
    printf(" -i | --ipadddress     : hostname or ipaddress\n");
    printf(" -t | --tcp            : tcp network transport\n");
    printf(" -u | --udp            : udp network transport\n");
    printf("\nOutputs: \n");
    printf(" -o | --output         : output incoming data to filename\n");
    printf("\nStatistics: \n");
    printf(" -m | --monitor        : monitor mode (run as unix daemon)\n");
    printf(" -s | --summary=[<type>]:write summary for one of the modes (default: bandwidth):\n");
    printf("                         bandwidth = bandwidth per elementary stream\n");
    printf("                         table  = tables and descriptors\n");
    printf("                         packet = decode packets and print structs\n");
//    printf("                         wire = print arrival time per packet (wireshark like)\n");
    printf("      --summary-file   : file to write summary information to (default: stdout)\n");
    printf("      --summary-period : refresh summary file every n milliseconds (default: 1000ms)\n");
#endif
    exit(EXIT_FAILURE);
}

/* Logging */
static int log_level[] = { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG };
static void log_remote(const int level, const char *format, ...)
{
    va_list ap;
    char *msg = NULL;
    va_start(ap, format);
    int err = vasprintf(&msg, format, ap);
    va_end(ap);
    if (err != 1)
    {
        syslog(log_level[level], "%s", msg);
        free(msg);
    }
}

static const char *psz_level[] = { "ERROR", "WARNING", "INFO", "DEBUG" };
static void log_local(const int level, const char *format, ...)
{
    va_list ap;
    int err = 0;
    char *msg = NULL;
    va_start(ap, format);
#if defined(_GNU_SOURCE)
    err = vasprintf(&msg, format, ap);
#else
    msg = calloc(1, 1024);
    if (msg)
        err = vsnprintf(msg, 1024, format, ap);
#endif
    va_end(ap);
    if (err != 1)
    {
        fprintf(stderr, "%s: %s", psz_level[level], msg);
        free(msg);
    }
}

static void libdvbpsi_log(void *data, const int level, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    params_t *param = (params_t *)data;
    if (param)
        param->pf_log(level, format, ap);
    va_end(ap);
}

/* Parameters */
static params_t *params_init(void)
{
    params_t *param;
    param = (params_t *) calloc(1, sizeof(params_t));
    if (param == NULL)
        exit(EXIT_FAILURE);

    param->fd_in = param->fd_out = -1;
    param->input = NULL;
    param->output = NULL;

    /* statistics */
    param->b_summary = false;
    param->summary.mode = SUM_BANDWIDTH;
    param->summary.file = NULL;
    param->summary.fd = stdout;
    param->summary.period = 1000;

    /* functions */
    param->pf_read = NULL;
    param->pf_write = NULL;
    param->pf_log = log_local;
    return param;
}

static void params_free(params_t *param)
{
    free(param->input);
    free(param->output);
    free(param->summary.file);
}

/* */
static void dvbinfo_close(params_t *param)
{
#ifdef HAVE_SYS_SOCKET_H
    if (param->input && param->b_udp)
        udp_close(param->fd_in);
    else if (param->input && param->b_tcp)
        tcp_close(param->fd_in);
    else
#endif
    if (param->input)
        close(param->fd_in);
    if (param->output)
        close(param->fd_out);
}

static void dvbinfo_open(params_t *param)
{
#ifdef HAVE_SYS_SOCKET_H
    if (param->output)
    {
        param->fd_out = open(param->output, O_CREAT | O_RDWR | O_NONBLOCK
                             | O_EXCL | O_CLOEXEC, S_IRWXU);
        if (param->fd_out < 0)
            goto error;
    }
    if (param->input && param->b_udp)
    {
        param->fd_in = udp_open(param->input, param->port);
        if (param->fd_in < 0)
            goto error;
    }
    else if (param->input && param->b_tcp)
    {
        param->fd_in = tcp_open(param->input, param->port);
        if (param->fd_in < 0)
            goto error;
    }
    else
#endif
    if (param->input)
    {
        param->fd_in = open(param->input, O_RDONLY | O_NONBLOCK);
        if (param->fd_in < 0)
            goto error;
    }
    return;

error:
    dvbinfo_close(param);
    params_free(param);
    exit(EXIT_FAILURE);
}

static void *dvbinfo_capture(void *data)
{
    dvbinfo_capture_t *capture = (dvbinfo_capture_t *)data;
    const params_t *param = capture->params;
    bool b_eof = false;

    while (capture->b_alive && !b_eof)
    {
        buffer_t *buffer;

        if (fifo_count(capture->empty) == 0)
            buffer = buffer_new(capture->size);
        else
            buffer = fifo_pop(capture->empty);

        if (buffer == NULL) /* out of memory */
            break;

        ssize_t size = param->pf_read(param->fd_in, buffer->p_data, buffer->i_size);
        if (size < 0) /* short read ? */
        {
            fifo_push(capture->empty, buffer);
            continue;
        }
        else if (size == 0)
        {
            fifo_push(capture->empty, buffer);
            b_eof = true;
            continue;
        }

        buffer->i_date = mdate();

        /* store buffer */
        fifo_push(capture->fifo, buffer);
        buffer = NULL;
    }

    capture->b_alive = false;
    fifo_wake(capture->fifo);
    return NULL;
}

static int dvbinfo_process(dvbinfo_capture_t *capture)
{
    int err = -1;
    bool b_error = false;
    params_t *param = capture->params;
    buffer_t *buffer = NULL;

    char *psz_temp = NULL;
    mtime_t deadline = 0;
    if (param->b_summary)
    {
        if (asprintf(&psz_temp, "%s.part", param->summary.file) < 0)
        {
            param->pf_log(DVBINFO_LOG_ERROR, "Could not create temporary summary file %s\n",
                          param->summary.file);
            return err;
        }
        deadline = mdate() + param->summary.period;
    }

    ts_stream_t *stream = libdvbpsi_init(param->debug, &libdvbpsi_log, (void *)param);
    if (!stream)
        goto out;

    while (!b_error)
    {
        /* Wait till fifo has emptied */
        if (!capture->b_alive && (fifo_count(capture->fifo) == 0))
            break;

        /* Wait for data to arrive */
        buffer = fifo_pop(capture->fifo);
        if (buffer == NULL)
        {
            if (capture->b_alive)
                continue;
            else
                break;
        }

        if (param->output)
        {
            size_t size = param->pf_write(param->fd_out, buffer->p_data, buffer->i_size);
            if (size < 0) /* error writing */
            {
                param->pf_log(DVBINFO_LOG_ERROR, "error (%d) writting to %s", errno, param->output);
                break;
            }
            else if (size < buffer->i_size) /* short writting disk full? */
            {
                param->pf_log(DVBINFO_LOG_ERROR, "error writting to %s (disk full?)", param->output);
                break;
            }
        }

        if (!libdvbpsi_process(stream, buffer->p_data, buffer->i_size, buffer->i_date))
            b_error = true;

        /* summary statistics */
        if (param->b_summary)
        {
            if (mdate() >= deadline)
            {
                FILE *fd = fopen(psz_temp, "w+");
                if (fd)
                {
                    libdvbpsi_summary(fd, stream, param->summary.mode);
                    fflush(fd);
                    fclose(fd);
                    unlink(param->summary.file);
                    rename(psz_temp, param->summary.file);
                }
                else
                {
                    param->pf_log(DVBINFO_LOG_ERROR, "failed opening summary file (disabling summary logging)\n");
                    param->b_summary = false;
                }
                deadline = mdate() + param->summary.period;
            }
        }

        /* reuse buffer */
        fifo_push(capture->empty, buffer);
        buffer = NULL;
    }

    libdvbpsi_exit(stream);
    err = 0;

out:
    if (b_error)
        param->pf_log(DVBINFO_LOG_ERROR, "error while processing\n" );

    if (buffer) buffer_free(buffer);
    free(psz_temp);
    return err;
}

/*
 * DVB Info main application
 */
int main(int argc, char **pp_argv)
{
    dvbinfo_capture_t capture;
    params_t *param = NULL;
    bool b_monitor = false;
    char c;

    printf("dvbinfo: Copyright (C) 2011-2012 M2X BV\n");
    printf("License: LGPL v2.1\n");

    if (argc == 1)
        usage();

    param = params_init();
    capture.params = param;
    capture.fifo = fifo_new();
    capture.empty = fifo_new();

    static const struct option long_options[] =
    {
        { "debug",     required_argument, NULL, 'd' },
        { "help",      no_argument,       NULL, 'h' },
        /* - inputs - */
        { "file",      required_argument, NULL, 'f' },
#ifdef HAVE_SYS_SOCKET_H
        { "ipaddress", required_argument, NULL, 'i' },
        { "tcp",       no_argument,       NULL, 't' },
        { "udp",       no_argument,       NULL, 'u' },
        /* - outputs - */
        { "output",    required_argument, NULL, 'o' },
        /* - daemon - */
        { "monitor",   no_argument,       NULL, 'm' },
        /* - statistics - */
        { "summary",        required_argument, NULL, 's' },
        { "summary-file",   required_argument, NULL, 'j' },
        { "summary-period", required_argument, NULL, 'p' },
#endif
        { NULL, 0, NULL, 0 }
    };
#ifdef HAVE_SYS_SOCKET_H
    while ((c = getopt_long(argc, pp_argv, "d:f:i:ho:ms:tu", long_options, NULL)) != -1)
#else
    while ((c = getopt_long(argc, pp_argv, "d:f:h", long_options, NULL)) != -1)
#endif
    {
        switch(c)
        {
            case 'd':
                if (optarg)
                {
                    param->debug = 0;
                    if (strncmp(optarg, "error", 5) == 0)
                        param->debug = 1;
                    else if (strncmp(optarg, "warn", 4) == 0)
                        param->debug = 2;
                    else if (strncmp(optarg, "debug", 5) == 0)
                        param->debug = 3;
                }
                break;

            case 'f':
                if (optarg)
                {
                    if (asprintf(&param->input, "%s", optarg) < 0)
                    {
                        fprintf(stderr, "error: out of memory\n");
                        params_free(param);
                        usage();
                    }
                    /* */
                    param->pf_read = read;
                }
                break;

#ifdef HAVE_SYS_SOCKET_H
            case 'i':
                if (optarg)
                {
                    char *psz_tmp = strtok(optarg,":");
                    if (psz_tmp)
                    {
                        size_t len = strlen(psz_tmp);
                        param->port = strtol(&optarg[len+1], NULL, 0);
                        param->input = strdup(psz_tmp);
                    }
                    else
                    {
                        params_free(param);
                        usage();
                    }
                }
                break;

            case 'm':
                b_monitor = true;
                param->pf_log = log_remote;
                break;

            case 'o':
                if (optarg)
                {
                    if (asprintf(&param->output, "%s", optarg) < 0)
                    {
                        fprintf(stderr, "error: out of memory\n");
                        params_free(param);
                        usage();
                    }
                    /* */
                    param->pf_write = write;
                }
                break;

            case 't':
                param->b_tcp = true;
                param->pf_read = tcp_read;
                break;

            case 'u':
                param->b_udp = true;
                param->pf_read = udp_read;
                break;

            /* - Statistics */
            case 's':
            {
                param->b_summary = true;
                ssize_t size = ARRAY_SIZE(psz_summary_mode);
                for (unsigned int i = 0; i < size; i++)
                {
                    printf("summary mode %s\n", psz_summary_mode[i]);
                    if (strncmp(optarg, psz_summary_mode[i], strlen(psz_summary_mode[i])) == 0)
                    {
                        param->summary.mode = i_summary_mode[i];
                        break;
                    }
                }
                break;
            }
            case 'j':
                if (optarg)
                {
                    if (asprintf(&param->summary.file, "%s", optarg) < 0)
                    {
                        params_free(param);
                        usage();
                    }
                }
                break;

            case 'p':
                {
                    char *end = NULL;
                    param->summary.period = strtoll(optarg, &end, 10);
                    if (((errno == ERANGE) &&
                            ((param->summary.period == LLONG_MIN) ||
                             (param->summary.period == LLONG_MAX))) ||
                        ((errno != 0) && (param->summary.period == 0)))
                    {
                        fprintf(stderr, "Option --summary-period has invalid content %s\n", optarg);
                        params_free(param);
                        usage();
                    }
                }
                break;
#endif
            case ':':
                fprintf(stderr, "Option %c is missing arguments\n", c);
                params_free(param);
                exit(EXIT_FAILURE);
                break;

            case '?':
               fprintf(stderr, "Unknown option %c found\n", c);
               params_free(param);
               exit(EXIT_FAILURE);
               break;

            case 'h':
            default:
                params_free(param);
                usage();
                break;
        }
    };

#ifdef HAVE_SYS_SOCKET_H
    if (b_monitor)
    {
        openlog("dvbinfo", LOG_PID, LOG_DAEMON);
        if (daemon(1,0) < 0)
        {
            param->pf_log(DVBINFO_LOG_ERROR, "Failed to start in background\n");
            params_free(param);
            closelog();
            usage(); /* exits application */
        }
        param->pf_log(DVBINFO_LOG_INFO, "dvbinfo: Copyright (C) 2011-2012 M2X BV\n");
        param->pf_log(DVBINFO_LOG_ERROR, "License: LGPL v2.1\n");
    }
#endif

    if (param->input == NULL)
    {
        param->pf_log(DVBINFO_LOG_ERROR, "No source given\n");
        params_free(param);
#ifdef HAVE_SYS_SOCKET_H
        if (b_monitor)
            closelog();
#endif
        params_free(param);
        usage(); /* exits application */
    }

#ifdef HAVE_SYS_SOCKET_H
    if (param->b_udp || param->b_tcp)
    {
        capture.size = 7*188;
        param->pf_log(DVBINFO_LOG_INFO, "Listen: host=%s port=%d\n", param->input, param->port);
    }
    else
#endif
    {
        capture.size = 188;
        param->pf_log(DVBINFO_LOG_INFO, "Examining: %s\n", param->input);
    }

    /* Capture thread */
    dvbinfo_open(param);
    pthread_t handle;
    capture.b_alive = true;
    if (pthread_create(&handle, NULL, dvbinfo_capture, (void *)&capture) < 0)
    {
        param->pf_log(DVBINFO_LOG_ERROR, "failed creating thread\n");
        dvbinfo_close(param);
#ifdef HAVE_SYS_SOCKET_H
        if (b_monitor)
            closelog();
#endif
        params_free(param);
        exit(EXIT_FAILURE);
    }
    int err = dvbinfo_process(&capture);
    capture.b_alive = false;     /* stop thread */
    if (pthread_join(handle, NULL) < 0)
        param->pf_log(DVBINFO_LOG_ERROR, "error joining capture thread\n");
    dvbinfo_close(param);

    /* cleanup */
    params_free(param);

    fifo_wake((&capture)->fifo);
    fifo_wake((&capture)->empty);

    fifo_free((&capture)->fifo);
    fifo_free((&capture)->empty);

#ifdef HAVE_SYS_SOCKET_H
    if (b_monitor)
        closelog();
#endif
    if (err < 0)
        exit(EXIT_FAILURE);
    else
        exit(EXIT_SUCCESS);
}
