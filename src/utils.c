/* $Id: utils.c,v 1.17 2001-11-22 00:31:10 rjkaes Exp $
 *
 * Misc. routines which are used by the various functions to handle strings
 * and memory allocation and pretty much anything else we can think of. Also,
 * the load cutoff routine is in here. Could not think of a better place for
 * it, so it's in here.
 *
 * Copyright (C) 1998  Steven Young
 * Copyright (C) 1999  Robert James Kaes (rjkaes@flarenet.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "tinyproxy.h"

#include "buffer.h"
#include "conns.h"
#include "log.h"
#include "sock.h"
#include "utils.h"

/*
 * These are the debugging calloc, malloc, and free versions
 */
#ifndef NDEBUG

void *
debugging_calloc(size_t nmemb, size_t size, const char *file,
		 unsigned long line)
{
	void *ptr = calloc(nmemb, size);
	fprintf(stderr, "{calloc: %p:%u x %u} %s:%lu\n", ptr, nmemb, size, file,
		line);
	return ptr;
}

void *
debugging_malloc(size_t size, const char *file, unsigned long line)
{
	void *ptr = malloc(size);
	fprintf(stderr, "{malloc: %p:%u} %s:%lu\n", ptr, size, file, line);
	return ptr;
}

void *
debugging_realloc(void *ptr, size_t size, const char *file, unsigned long line)
{
	void *newptr = realloc(ptr, size);
	fprintf(stderr, "{realloc: %p -> %p:%u} %s:%lu\n", ptr, newptr, size,
		file, line);
	return newptr;
}

void
debugging_free(void *ptr, const char *file, unsigned long line)
{
	fprintf(stderr, "{free: %p} %s:%lu\n", ptr, file, line);
	free(ptr);
	return;
}

#endif

#define HEADER_SIZE (1024 * 8)
/*
 * Build the data for a complete HTTP & HTML message for the client.
 */
int
send_http_message(struct conn_s *connptr, int http_code,
		  const char *error_title, const char *message)
{
	static char *headers =
	    "HTTP/1.0 %d %s\r\n"
	    "Server: %s/%s\r\n"
	    "Date: %s\r\n"
	    "Content-Type: text/html\r\n"
	    "Content-Length: %d\r\n" "Connection: close\r\n" "\r\n";

	char *header_buffer;
	char timebuf[30];
	time_t global_time;

	header_buffer = safemalloc(HEADER_SIZE);
	if (!header_buffer)
		return -1;

	global_time = time(NULL);
	strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S GMT",
		 gmtime(&global_time));

	snprintf(header_buffer, HEADER_SIZE - 1, headers, http_code,
		 error_title, PACKAGE, VERSION, timebuf, strlen(message));

	safe_write(connptr->client_fd, header_buffer, strlen(header_buffer));
	safe_write(connptr->client_fd, message, strlen(message));

	safefree(header_buffer);

	connptr->send_message = TRUE;

	return 0;
}

/*
 * Display an error to the client.
 */
int
httperr(struct conn_s *connptr, int err, const char *msg)
{
	static char *message =
	    "<html><head><title>%s</title></head>\r\n"
	    "<body>\r\n"
	    "<font size=\"+2\">Cache Error!</font><br>\r\n"
	    "An error of type %d occurred: %s\r\n"
	    "<hr>\r\n"
	    "<font size=\"-1\"><em>Generated by %s (%s)</em></font>\r\n"
	    "</body></html>\r\n\r\n";

	char *message_buffer;

	message_buffer = safemalloc(MAXBUFFSIZE);
	if (!message_buffer)
		return -1;

	snprintf(message_buffer, MAXBUFFSIZE - 1, message, msg, err, msg,
		 PACKAGE, VERSION);

	if (send_http_message(connptr, err, msg, message_buffer) < 0) {
		safefree(message_buffer);
		return -1;
	}

	safefree(message_buffer);
	return 0;
}

void
makedaemon(void)
{
	if (fork() != 0)
		exit(0);

	setsid();
	signal(SIGHUP, SIG_IGN);

	if (fork() != 0)
		exit(0);

	chdir("/");
	umask(077);

	close(0);
	close(1);
	close(2);
}

/*
 * Safely creates filename and returns the low-level file descriptor.
 */
int
create_file_safely(const char *filename)
{
	struct stat lstatinfo;
	int fildes;

	/*
	 * lstat() the file. If it doesn't exist, create it with O_EXCL.
	 * If it does exist, open it for writing and perform the fstat()
	 * check.
	 */
	if (lstat(filename, &lstatinfo) < 0) {
		/*
		 * If lstat() failed for any reason other than "file not
		 * existing", exit.
		 */
		if (errno != ENOENT) {
			log_message(LOG_ERR,
				    "create_file_safely: Error checking PID file %s: %s.",
				    filename, strerror(errno));
			return -1;
		}

		/*
		 * The file doesn't exist, so create it with O_EXCL to make
		 * sure an attacker can't slip in a file between the lstat()
		 * and open()
		 */
		if ((fildes =
		     open(filename, O_RDWR | O_CREAT | O_EXCL, 0600)) < 0) {
			log_message(LOG_ERR,
				    "create_file_safely: Could not create PID file %s: %s.",
				    filename, strerror(errno));
			return -1;
		}
	} else {
		struct stat fstatinfo;

		/*
		 * Open an existing file.
		 */
		if ((fildes = open(filename, O_RDWR)) < 0) {
			log_message(LOG_ERR,
				    "create_file_safely: Could not open PID file %s: %s.",
				    filename, strerror(errno));
			return -1;
		}

		/*
		 * fstat() the opened file and check that the file mode bits,
		 * inode, and device match.
		 */
		if (fstat(fildes, &fstatinfo) < 0
		    || lstatinfo.st_mode != fstatinfo.st_mode
		    || lstatinfo.st_ino != fstatinfo.st_ino
		    || lstatinfo.st_dev != fstatinfo.st_dev) {
			log_message(LOG_ERR,
				    "create_file_safely: The PID file %s has been changed before it could be opened.",
				    filename);
			close(fildes);
			return -1;
		}

		/*
		 * If the above check was passed, we know that the lstat()
		 * and fstat() were done on the same file. Now we check that
		 * there's only one link, and that it's a normal file (this
		 * isn't strictly necessary because the fstat() vs lstat()
		 * st_mode check would also find this)
		 */
		if (fstatinfo.st_nlink > 1 || !S_ISREG(lstatinfo.st_mode)) {
			log_message(LOG_ERR,
				    "create_file_safely: The PID file %s has too many links, or is not a regular file: %s.",
				    filename, strerror(errno));
			close(fildes);
			return -1;
		}

		/*
		 * On systems whcih don't support ftruncate() the best we can
		 * do is to close the file and reopen it in create mode, which
		 * unfortunately leads to a race condition, however "systems
		 * which don't support ftruncate()" is pretty much SCO only,
		 * and if you're using that you deserve what you get.
		 * ("Little sympathy has been extended")
		 */
#ifdef HAVE_FTRUNCATE
		ftruncate(fildes, 0);
#else
		close(fildes);
		if ((fildes =
		     open(filename, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0) {
			log_message(LOG_ERR,
				    "create_file_safely: Could not open PID file %s: %s.",
				    filename, strerror(errno));
			return -1;
		}
#endif				/* HAVE_FTRUNCATE */
	}

	return fildes;
}

/*
 * Write the PID of the program to the specified file.
 */
void
pidfile_create(const char *filename)
{
	int fildes;
	FILE *fd;

	/*
	 * Create a new file
	 */
	if ((fildes = create_file_safely(filename)) < 0)
		exit(1);

	/*
	 * Open a stdio file over the low-level one.
	 */
	if ((fd = fdopen(fildes, "w")) == NULL) {
		log_message(LOG_ERR,
			    "pidfile_create: fdopen() error on PID file %s: %s.",
			    filename, strerror(errno));
		close(fildes);
		unlink(filename);
		exit(1);
	}

	fprintf(fd, "%ld\n", (long) getpid());
	fclose(fd);
}

#ifndef HAVE_STRLCPY
/*
 * Function API taken from OpenBSD. Like strncpy(), but does not 0 fill the
 * buffer, and always NULL terminates the buffer. size is the size of the
 * destination buffer.
 */
size_t
strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);
	size_t ret = len;

	if (len >= size)
		len = size - 1;

	memcpy(dst, src, len);
	dst[len] = '\0';

	return ret;
}
#endif

#ifndef HAVE_STRLCAT
/*
 * Function API taken from OpenBSD. Like strncat(), but does not 0 fill the
 * buffer, and always NULL terminates the buffer. size is the length of the
 * buffer, which should be one more than the maximum resulting string
 * length.
 */
size_t
strlcat(char *dst, const char *src, size_t size)
{
	size_t len1 = strlen(dst);
	size_t len2 = strlen(src);
	size_t ret = len1 + len2;

	if (len1 + len2 >= size)
		len2 = size - len1 - 1;
	if (len2 > 0) {
		memcpy(dst + len1, src, len2);
		dst[len1 + len2] = '\0';
	}

	return ret;
}
#endif
