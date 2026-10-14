/*
 * phantom_art.c - see phantom_art.h. A small HTTP/1.0 client that never waits: one GET, one response, a megabyte of body.
 */
#include "phantom_art.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum { S_IDLE = 0, S_WAIT, S_RETRY, S_CONNECTING, S_SENDING, S_HEAD, S_BODY };

/* A step that has taken this long has failed: the host is gone, or a datagram's worth of it is. */
#define PA_STEP_TIMEOUT_MS   8000
/* Asks while the host says "fetching" (202): every 1.5 s for a minute. */
#define PA_PENDING_RETRY_MS  1500
#define PA_PENDING_TRIES     40
/* Quick tries after a network failure, each a second later than the last; after that the host is off or restarting, and the
 * row is asked for again every PA_SLOW_RETRY_MS for as long as the cursor stays on it, so it is there when the host is back. */
#define PA_FAIL_TRIES        4
#define PA_SLOW_RETRY_MS     15000
/* The most read in one call, so a megabyte does not hold up the rest of the poll loop. */
#define PA_READ_BURST        (256 * 1024)

void pa_init(pa_art *a)
{
	memset(a, 0, sizeof(*a));
	a->fd = -1;
	a->port = 8088;
	a->status = PA_NOTHING;
}

static void close_fd(pa_art *a)
{
	if (a->fd >= 0)
	{
		close(a->fd);
		a->fd = -1;
	}
}

static void reset_transfer(pa_art *a)
{
	close_fd(a);
	a->state = S_IDLE;
	a->head_len = 0;
	a->body_got = 0;
	a->req_sent = 0;
	a->req_len = 0;
}

void pa_free(pa_art *a)
{
	close_fd(a);
	free(a->body);
	free(a->pic);
	a->body = 0;
	a->pic = 0;
	a->pic_key[0] = 0;
}

void pa_set_host(pa_art *a, const char *ip, int port)
{
	if (!ip) ip = "";
	if (port <= 0 || port > 65535) port = 8088;
	if (!strcmp(a->host, ip) && a->port == port) return;
	snprintf(a->host, sizeof(a->host), "%s", ip);
	a->port = port;
	reset_transfer(a);
	a->tries = 0;
	a->fails = 0;
	if (a->want)
	{
		a->state = S_WAIT;
		a->want_since = 0;   /* the cursor has been here all along; do not wait again */
	}
}

void pa_want(pa_art *a, const char *key, const char *query, uint32_t now)
{
	if (!key || !query)
	{
		if (a->want)
		{
			a->want = 0;
			a->want_key[0] = 0;
			reset_transfer(a);
			a->status = PA_NOTHING;
		}
		return;
	}
	if (a->want && !strcmp(a->want_key, key)) return;

	a->want = 1;
	snprintf(a->want_key, sizeof(a->want_key), "%s", key);
	snprintf(a->want_query, sizeof(a->want_query), "%s", query);
	a->want_since = now;
	a->tries = 0;
	a->fails = 0;
	reset_transfer(a);

	if (a->pic && !strcmp(a->pic_key, key))
	{
		a->status = PA_READY;   /* the picture it already holds is this row's */
		return;
	}
	a->status = PA_NOTHING;
	a->state = S_WAIT;
}

static int fail(pa_art *a, uint32_t now)
{
	reset_transfer(a);
	a->fails++;
	a->state = S_RETRY;
	a->retry_at = now + (a->fails >= PA_FAIL_TRIES ? (uint32_t)PA_SLOW_RETRY_MS : 1000u * (uint32_t)a->fails);
	return 0;
}

static void start_connect(pa_art *a, uint32_t now)
{
	reset_transfer(a);

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons((uint16_t)a->port);
	to.sin_addr.s_addr = inet_addr(a->host);
	if (!a->host[0] || to.sin_addr.s_addr == INADDR_NONE)
	{
		a->state = S_IDLE;   /* no host to ask */
		return;
	}

	a->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (a->fd < 0)
	{
		fail(a, now);
		return;
	}
	fcntl(a->fd, F_SETFL, O_NONBLOCK);

	a->req_len = snprintf(a->req, sizeof(a->req), "GET /art?%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
	                      a->want_query, a->host);
	a->req_sent = 0;
	a->head_len = 0;
	a->body_got = 0;
	a->state_t0 = now;

	int rc = connect(a->fd, (struct sockaddr *)&to, sizeof(to));
	if (rc == 0 || errno == EINPROGRESS) a->state = S_CONNECTING;
	else fail(a, now);
}

/* The value of a header (case-insensitive name, ends in ':'), or -1. Only numbers are asked for here. */
static long header_number(const char *head, const char *name)
{
	const size_t n = strlen(name);
	for (const char *p = head; *p; p++)
	{
		if ((p == head || p[-1] == '\n') && !strncasecmp(p, name, n))
		{
			const char *v = p + n;
			while (*v == ' ') v++;
			return strtol(v, 0, 10);
		}
	}
	return -1;
}

int pa_tick(pa_art *a, uint32_t now)
{
	if (!a->want) return 0;
	int changed = 0;

	switch (a->state)
	{
	case S_IDLE:
		return 0;

	case S_WAIT:
		if ((int32_t)(now - a->want_since) >= PA_DEBOUNCE_MS) start_connect(a, now);
		return 0;

	case S_RETRY:
		if ((int32_t)(now - a->retry_at) >= 0) start_connect(a, now);
		return 0;

	default:
		break;
	}

	/* a step in progress */
	if ((int32_t)(now - a->state_t0) > PA_STEP_TIMEOUT_MS) return fail(a, now);

	if (a->state == S_CONNECTING)
	{
		struct pollfd p = { a->fd, POLLOUT, 0 };
		if (poll(&p, 1, 0) <= 0) return 0;
		int err = 0;
		socklen_t el = sizeof(err);
		if (getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) return fail(a, now);
		a->state = S_SENDING;
		a->state_t0 = now;
	}

	if (a->state == S_SENDING)
	{
		while (a->req_sent < a->req_len)
		{
			ssize_t n = send(a->fd, a->req + a->req_sent, (size_t)(a->req_len - a->req_sent), MSG_NOSIGNAL);
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
				return fail(a, now);
			}
			a->req_sent += (int)n;
		}
		a->state = S_HEAD;
		a->state_t0 = now;
	}

	if (a->state == S_HEAD)
	{
		for (;;)
		{
			if (a->head_len >= (int)sizeof(a->head) - 1) return fail(a, now);   /* no end to the headers: not what we asked */
			ssize_t n = recv(a->fd, a->head + a->head_len, sizeof(a->head) - 1 - (size_t)a->head_len, 0);
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
				return fail(a, now);
			}
			if (n == 0) return fail(a, now);
			a->head_len += (int)n;
			a->head[a->head_len] = 0;
			char *end = strstr(a->head, "\r\n\r\n");
			if (!end) continue;

			int status = 0;
			if (sscanf(a->head, "HTTP/%*d.%*d %d", &status) != 1) return fail(a, now);
			a->http_status = status;
			const long clen = header_number(a->head, "content-length:");
			const int body_at = (int)(end + 4 - a->head);

			if (status == 200 && clen == PA_BYTES)
			{
				if (!a->body) a->body = (uint8_t *)malloc(PA_BYTES);
				if (!a->body) return fail(a, now);
				a->content_len = clen;
				long have = a->head_len - body_at;
				if (have > PA_BYTES) have = PA_BYTES;
				if (have > 0) memcpy(a->body, a->head + body_at, (size_t)have);
				a->body_got = have;
				a->state = S_BODY;
				a->state_t0 = now;
				break;
			}

			close_fd(a);
			a->head_len = 0;
			if (status == 202)
			{
				a->fails = 0;   /* a real answer about this row: the network is fine */
				if (a->status != PA_PENDING) changed = 1;
				a->status = PA_PENDING;
				if (++a->tries > PA_PENDING_TRIES)
				{
					a->state = S_IDLE;
					return changed;
				}
				a->state = S_RETRY;
				a->retry_at = now + PA_PENDING_RETRY_MS;
				return changed;
			}
			if (status == 404)
			{
				a->fails = 0;
				if (a->status != PA_NONE) changed = 1;
				a->status = PA_NONE;
				a->state = S_IDLE;
				return changed;
			}
			if (status == 409)
			{
				a->stale = 1;
				a->state = S_IDLE;
				return 0;
			}
			return fail(a, now);   /* 400, 503, anything else: not an answer about this row */
		}
	}

	if (a->state == S_BODY)
	{
		long budget = PA_READ_BURST;
		while (a->body_got < PA_BYTES && budget > 0)
		{
			ssize_t n = recv(a->fd, a->body + a->body_got, (size_t)(PA_BYTES - a->body_got), 0);
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
				return fail(a, now);
			}
			if (n == 0) return fail(a, now);   /* closed with the picture unfinished */
			a->body_got += n;
			budget -= n;
			a->state_t0 = now;   /* it is moving: the timeout is for a stall, not for a megabyte */
		}
		if (a->body_got >= PA_BYTES)
		{
			if (!a->pic) a->pic = (uint8_t *)malloc(PA_BYTES);
			if (!a->pic) return fail(a, now);
			memcpy(a->pic, a->body, PA_BYTES);
			snprintf(a->pic_key, sizeof(a->pic_key), "%s", a->want_key);
			a->pics++;
			a->status = PA_READY;
			reset_transfer(a);
			return 1;
		}
	}
	return changed;
}

pa_status pa_get_status(const pa_art *a)
{
	return a->want ? a->status : PA_NOTHING;
}

const uint8_t *pa_picture(const pa_art *a)
{
	return a->pic;
}

const char *pa_picture_key(const pa_art *a)
{
	return a->pic_key;
}

int pa_take_stale(pa_art *a)
{
	const int s = a->stale;
	a->stale = 0;
	return s;
}
