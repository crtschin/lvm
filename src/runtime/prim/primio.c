/***********************************************************************/
/*                                                                     */
/*                           Objective Caml                            */
/*                                                                     */
/*            Xavier Leroy, projet Cristal, INRIA Rocquencourt         */
/*                                                                     */
/*  Copyright 1996 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License.         */
/*                                                                     */
/***********************************************************************/

/***---------------------------------------------------------------------
  Modified and adapted for the Lazy Virtual Machine by Daan Leijen.
  Modifications copyright 2001, Daan Leijen. This (modified) file is
  distributed under the terms of the GNU Library General Public License.
---------------------------------------------------------------------***/

/* $Id$ */

/* Buffered input/output. */

#include <errno.h>
#include <limits.h>
#include <string.h>
#ifdef _WIN32
# include <fcntl.h>
#endif

#include "mlvalues.h"
#include "fail.h"
#include "memory.h"
#include "alloc.h"
#include "custom.h"
#include "fail.h"

#ifdef HAS_IO_H
#include <io.h>
#endif
#ifdef HAS_UNISTD_H
#include <unistd.h>
#endif

#include "primio.h"
#include "primsys.h"  /* NO_ARG */

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define Enter_blocking_section()
#define Leave_blocking_section()

/* Hooks for locking channels */

void (*channel_mutex_free) (struct channel *) = NULL;
void (*channel_mutex_lock) (struct channel *) = NULL;
void (*channel_mutex_unlock) (struct channel *) = NULL;
void (*channel_mutex_unlock_exn) (void) = NULL;

/* Basic functions over type struct channel *.
   These functions can be called directly from C.
   No locking is performed. */

/* Functions shared between input and output */

struct channel * open_descriptor(int fd, bool output)
{
  struct channel * channel;

  channel = (struct channel *) stat_alloc(sizeof(struct channel));
  channel->fd = fd;
  channel->output = output;
  channel->offset = 0;
  channel->curr = channel->max = channel->buff;
  channel->end = channel->buff + IO_BUFFER_SIZE;
  channel->mutex = NULL;
  return channel;
}

void close_channel(struct channel *channel)
{
  close(channel->fd);
  if (channel_mutex_free != NULL) (*channel_mutex_free)(channel);
  stat_free(channel);
}

long channel_size(struct channel *channel)
{
  long end;

  end = lseek(channel->fd, 0, SEEK_END);
  if (end == -1 ||
      lseek(channel->fd, channel->offset, SEEK_SET) != channel->offset) {
    sys_error(NO_ARG);
  }
  return end;
}

int channel_binary_mode(struct channel *channel)
{
#ifdef _WIN32
  int oldmode = setmode(channel->fd, O_BINARY);
  if (oldmode == O_TEXT) setmode(channel->fd, O_TEXT);
  return oldmode == O_BINARY;
#else
  return 1;
#endif
}

/* Output */

#ifndef EINTR
#define EINTR (-1)
#endif
#ifndef EAGAIN
#define EAGAIN (-1)
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK (-1)
#endif

static int do_write(int fd, char *p, int n)
{
  int retcode;

/*  Assert(!Is_young(p)); */
#ifdef HAS_UI
  retcode = ui_write(fd, p, n);
#else
again:
  Enter_blocking_section();
  retcode = write(fd, p, n);
  Leave_blocking_section();
  if (retcode == -1) {
    if (errno == EINTR) goto again;
    if ((errno == EAGAIN || errno == EWOULDBLOCK) && n > 1) {
      /* We couldn't do a partial write here, probably because
         n <= PIPE_BUF and POSIX says that writes of less than
         PIPE_BUF characters must be atomic.
         We first try again with a partial write of 1 character.
         If that fails too, we'll raise Sys_blocked_io below. */
      n = 1; goto again;
    }
  }
#endif
  if (retcode == -1) sys_error(NO_ARG);
  return retcode;
}

/* Attempt to flush the buffer. This will make room in the buffer for
   at least one character. Returns true if the buffer is empty at the
   end of the flush, or false if some data remains in the buffer. */

int flush_partial(struct channel *channel)
{
  int towrite, written;

  towrite = channel->curr - channel->buff;
  if (towrite > 0) {
    written = do_write(channel->fd, channel->buff, towrite);
    channel->offset += written;
    if (written < towrite)
      memmove(channel->buff, channel->buff + written, towrite - written);
    channel->curr -= written;
  }
  return (channel->curr == channel->buff);
}

/* Flush completely the buffer. */

void flush(struct channel *channel)
{
  while (! flush_partial(channel)) /*nothing*/;
}

/* Output data */

void putword(struct channel *channel, uint32 w)
{
  if (! channel_binary_mode(channel))
    raise_user("output_binary_int: not a binary channel");
  putch(channel, w >> 24);
  putch(channel, w >> 16);
  putch(channel, w >> 8);
  putch(channel, w);
}

int putblock(struct channel *channel, const char *p, long int len)
{
  int n, free, towrite, written;

  n = len >= INT_MAX ? INT_MAX : (int) len;
  free = channel->end - channel->curr;
  if (n <= free) {
    /* Write request small enough to fit in buffer: transfer to buffer. */
    memmove(channel->curr, p, n);
    channel->curr += n;
    return n;
  } else {
    /* Write request overflows buffer: transfer whatever fits to buffer
       and write the buffer */
    memmove(channel->curr, p, free);
    towrite = channel->end - channel->buff;
    written = do_write(channel->fd, channel->buff, towrite);
    if (written < towrite)
      memmove(channel->buff, channel->buff + written, towrite - written);
    channel->offset += written;
    channel->curr = channel->end - written;
    channel->max = channel->end - written;
    return free;
  }
}

void really_putblock(struct channel *channel, const char *p, long int len)
{
  int written;
  while (len > 0) {
    written = putblock(channel, p, len);
    p += written;
    len -= written;
  }
}

void seek_out(struct channel *channel, long int dest)
{
  flush(channel);
  if (lseek(channel->fd, dest, 0) != dest) sys_error(NO_ARG);
  channel->offset = dest;
}

long pos_out(struct channel *channel)
{
  return channel->offset + channel->curr - channel->buff;
}

/* Input */

static int do_read(int fd, char *p, unsigned int n)
{
  int retcode;

/*  Assert(!Is_young(p)); */
  Enter_blocking_section();
#ifdef HAS_UI
  retcode = ui_read(fd, p, n);
#else
#ifdef EINTR
  do { retcode = read(fd, p, n); } while (retcode == -1 && errno == EINTR);
#else
  retcode = read(fd, p, n);
#endif
#endif
  Leave_blocking_section();
  if (retcode == -1) sys_error(NO_ARG);
  return retcode;
}

unsigned char refill(struct channel *channel)
{
  int n;

  n = do_read(channel->fd, channel->buff, IO_BUFFER_SIZE);
  if (n == 0) { raise_eof_exn(); }
  channel->offset += n;
  channel->max = channel->buff + n;
  channel->curr = channel->buff + 1;
  return (unsigned char)(channel->buff[0]);
}

uint32 getword(struct channel *channel)
{
  int i;
  uint32 res;

  if (! channel_binary_mode(channel))
    raise_user("input_binary_int: not a binary channel");
  res = 0;
  for(i = 0; i < 4; i++) {
    res = (res << 8) + getch(channel);
  }
  return res;
}

int getblock(struct channel *channel, char *p, long int len)
{
  int n, avail, nread;

  n = len >= INT_MAX ? INT_MAX : (int) len;
  avail = channel->max - channel->curr;
  if (n <= avail) {
    memmove(p, channel->curr, n);
    channel->curr += n;
    return n;
  } else if (avail > 0) {
    memmove(p, channel->curr, avail);
    channel->curr += avail;
    return avail;
  } else {
    nread = do_read(channel->fd, channel->buff, IO_BUFFER_SIZE);
    channel->offset += nread;
    channel->max = channel->buff + nread;
    if (n > nread) n = nread;
    memmove(p, channel->buff, n);
    channel->curr = channel->buff + n;
    return n;
  }
}

int really_getblock(struct channel *chan, char *p, long int n)
{
  int r;
  while (n > 0) {
    r = getblock(chan, p, n);
    if (r == 0) break;
    p += r;
    n -= r;
  }
  return (n == 0);
}

void seek_in(struct channel *channel, long int dest)
{
  if (dest >= channel->offset - (channel->max - channel->buff) &&
      dest <= channel->offset) {
    channel->curr = channel->max - (channel->offset - dest);
  } else {
    if (lseek(channel->fd, dest, SEEK_SET) != dest) sys_error(NO_ARG);
    channel->offset = dest;
    channel->curr = channel->max = channel->buff;
  }
}

long pos_in(struct channel *channel)
{
  return channel->offset - (channel->max - channel->curr);
}

long input_scan_line(struct channel *channel)
{
  char * p;
  int n;

  p = channel->curr;
  do {
    if (p >= channel->max) {
      /* No more characters available in the buffer */
      if (channel->curr > channel->buff) {
        /* Try to make some room in the buffer by shifting the unread
           portion at the beginning */
        memmove(channel->buff, channel->curr, channel->max - channel->curr);
        n = channel->curr - channel->buff;
        channel->curr -= n;
        channel->max -= n;
        p -= n;
      }
      if (channel->max >= channel->end) {
        /* Buffer is full, no room to read more characters from the input.
           Return the number of characters in the buffer, with negative
           sign to indicate that no newline was encountered. */
        return -(channel->max - channel->curr);
      }
      /* Fill the buffer as much as possible */
      n = do_read(channel->fd, channel->max, channel->end - channel->max);
      if (n == 0) {
        /* End-of-file encountered. Return the number of characters in the
           buffer, with negative sign since we haven't encountered
           a newline. */
        return -(channel->max - channel->curr);
      }
      channel->offset += n;
      channel->max += n;
    }
  } while (*p++ != '\n');
  /* Found a newline. Return the length of the line, newline included. */
  return (p - channel->curr);
}

/* Caml entry points for the I/O functions.  Wrap struct channel *
   objects into a heap-allocated object.  Perform locking
   and unlocking around the I/O operations. */

static void finalize_channel(value vchan)
{
  struct channel * chan = Channel(vchan);
  /*LVM: flush on finalize */
  if (chan->fd != -1 && chan->output) {
    Lock(chan);
    flush(chan);
    Unlock(chan);
  }
  if (chan->fd != -1) close(chan->fd);
  if (channel_mutex_free != NULL) (*channel_mutex_free)(chan);
  stat_free(chan);
}

static int compare_channel(value vchan1, value vchan2)
{
  struct channel * chan1 = Channel(vchan1);
  struct channel * chan2 = Channel(vchan2);
  return (chan1 == chan2) ? 0 : (chan1 < chan2) ? -1 : 1;
}

static struct custom_operations channel_operations = {
  "_chan",
  finalize_channel,
  compare_channel,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

static value alloc_channel(struct channel *chan)
{
  value res = alloc_custom(&channel_operations, sizeof(struct channel *),
                           1, 1000);
  Channel(res) = chan;
  return res;
}


/*----------------------------------------------------------------------
primitives
----------------------------------------------------------------------*/

value prim_open_descriptor(long fd, bool output)
{
  return alloc_channel(open_descriptor(fd,output));
}

value channel_descriptor(value vchannel)   /* ML */
{
  int fd = Channel(vchannel)->fd;
  if (fd == -1) { errno = EBADF; sys_error(NO_ARG); }
  return Val_int(fd);
}

void prim_close_channel(value vchannel)
{
  /* For output channels, must have flushed before */
  struct channel * channel = Channel(vchannel);
/* LVM: flush before close */
  if (channel->fd != -1 && channel->output) {
    Lock(channel);
    flush(channel);
    Unlock(channel);
  }
  close(channel->fd);
  channel->fd = -1;
  /* Ensure that every read or write on the channel will cause an
     immediate flush_partial or refill, thus raising a Sys_error
     exception */
  channel->curr = channel->max = channel->end;
  return;
}

value caml_channel_size(value vchannel)      /* ML */
{
  return Val_long(channel_size(Channel(vchannel)));
}

void prim_set_binary_mode(value vchannel, bool mode) /* ML */
{
#ifdef _WIN32
  struct channel * channel = Channel(vchannel);
  if (setmode(channel->fd, mode? O_BINARY : O_TEXT) == -1)
    sys_error(NO_ARG);
#endif
  return;
}

bool prim_flush_partial(value vchannel)            /* ML */
{
  struct channel * channel = Channel(vchannel);
  int res;
  /* Don't fail if channel is closed, this causes problem with flush on
     stdout and stderr at exit.  Revise when "flushall" is implemented. */
  if (channel->fd == -1) return Val_true;
  Lock(channel);
  res = flush_partial(channel);
  Unlock(channel);
  return res;
}

void prim_flush(value vchannel)
{
  struct channel * channel = Channel(vchannel);
  /* Don't fail if channel is closed, this causes problem with flush on
     stdout and stderr at exit.  Revise when "flushall" is implemented. */
  if (channel->fd == -1) return;
  Lock(channel);
  flush(channel);
  Unlock(channel);
  return;
}

void prim_output_char(value vchannel, char ch)
{
  struct channel * channel = Channel(vchannel);
  Lock(channel);
  putch(channel, ch);
  Unlock(channel);
  return;
}

value caml_output_int(value vchannel, value w)    /* ML */
{
  struct channel * channel = Channel(vchannel);
  Lock(channel);
  putword(channel, Long_val(w));
  Unlock(channel);
  return Val_unit;
}

value caml_output_partial(value vchannel, value buff, value start, value length) /* ML */
{
  CAMLparam4 (vchannel, buff, start, length);
  struct channel * channel = Channel(vchannel);
  int res;

  Lock(channel);
  res = putblock(channel, &Byte(buff, Long_val(start)), Long_val(length));
  Unlock(channel);
  CAMLreturn (Val_int(res));
}

value prim_output(value vchannel, const char* buff, long start, long length)
{
  CAMLparam1 (vchannel );
  struct channel * channel = Channel(vchannel);
  long pos = start;
  long len = length;

  Lock(channel);
    while (len > 0) {
      int written = putblock(channel, buff+pos, len);
      pos += written;
      len -= written;
    }
  Unlock(channel);
  CAMLreturn (Val_unit);
}

value caml_seek_out(value vchannel, value pos)    /* ML */
{
  struct channel * channel = Channel(vchannel);
  Lock(channel);
  seek_out(channel, Long_val(pos));
  Unlock(channel);
  return Val_unit;
}

value caml_pos_out(value vchannel)          /* ML */
{
  return Val_long(pos_out(Channel(vchannel)));
}

long prim_input_char(value vchannel)
{
  struct channel * channel = Channel(vchannel);
  unsigned char c;

  Lock(channel);
  c = getch(channel);
  Unlock(channel);
  return c;
}

value caml_input_int(value vchannel)        /* ML */
{
  struct channel * channel = Channel(vchannel);
  long i;

  Lock(channel);
  i = getword(channel);
  Unlock(channel);
#ifdef ARCH_SIXTYFOUR
  i = (i << 32) >> 32;          /* Force sign extension */
#endif
  return Val_long(i);
}

value caml_input(value vchannel,value buff,value vstart,value vlength) /* ML */
{
  CAMLparam4 (vchannel, buff, vstart, vlength);
  struct channel * channel = Channel(vchannel);
  long start, len;
  int n, avail, nread;

  Lock(channel);
  /* We cannot call getblock here because buff may move during do_read */
  start = Long_val(vstart);
  len = Long_val(vlength);
  n = len >= INT_MAX ? INT_MAX : (int) len;
  avail = channel->max - channel->curr;
  if (n <= avail) {
    memmove(&Byte(buff, start), channel->curr, n);
    channel->curr += n;
  } else if (avail > 0) {
    memmove(&Byte(buff, start), channel->curr, avail);
    channel->curr += avail;
    n = avail;
  } else {
    nread = do_read(channel->fd, channel->buff, IO_BUFFER_SIZE);
    channel->offset += nread;
    channel->max = channel->buff + nread;
    if (n > nread) n = nread;
    memmove(&Byte(buff, start), channel->buff, n);
    channel->curr = channel->buff + n;
  }
  Unlock(channel);
  CAMLreturn (Val_long(n));
}

value caml_seek_in(value vchannel, value pos)     /* ML */
{
  struct channel * channel = Channel(vchannel);
  Lock(channel);
  seek_in(channel, Long_val(pos));
  Unlock(channel);
  return Val_unit;
}

value caml_pos_in(value vchannel)           /* ML */
{
  return Val_long(pos_in(Channel(vchannel)));
}

value caml_input_scan_line(value vchannel)       /* ML */
{
  struct channel * channel = Channel(vchannel);
  long res;

  Lock(channel);
  res = input_scan_line(channel);
  Unlock(channel);
  return Val_long(res);
}
