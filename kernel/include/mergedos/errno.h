/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_ERRNO_H
#define MERGEDOS_ERRNO_H

/*
 * Native MergedOS error codes.
 *
 * Kernel services return 0 / a non-negative value on success and -MERR_*
 * on failure. These numbers deliberately match no foreign ABI: every
 * personality translates them into its own dialect on the way out
 * (Linux errno, BSD errno + carry flag, NTSTATUS) in abi_dispatch/errno_map.c.
 */
enum merr {
	MERR_OK = 0,
	MERR_PERM,      /* operation not permitted            */
	MERR_NOENT,     /* no such object                     */
	MERR_IO,        /* I/O error                          */
	MERR_BADF,      /* bad descriptor / handle            */
	MERR_NOMEM,     /* out of memory                      */
	MERR_ACCES,     /* access denied                      */
	MERR_FAULT,     /* bad user-space address             */
	MERR_INVAL,     /* invalid argument                   */
	MERR_NOEXEC,    /* not a loadable image               */
	MERR_NOSYS,     /* system call not implemented        */
	MERR_NOTSUP,    /* recognised but unsupported feature */
	MERR_AGAIN,     /* try again                          */
	MERR_RANGE,     /* value or buffer too large          */
	MERR_NOTTY,     /* not a terminal                     */
	MERR_NOTDIR,    /* not a directory                    */
	MERR_ISDIR,     /* is a directory                     */
	MERR_EXIST,     /* already exists                     */
	MERR_NOTEMPTY,  /* directory not empty                */
	MERR_CHILD,     /* no child process to wait for       */
	MERR_INTR,      /* interrupted by a signal            */
	MERR_PIPE,      /* write to a pipe nobody reads       */
	MERR_SRCH,      /* no such process                    */
	MERR_TIMEDOUT,  /* wait timed out                     */
	MERR_MFILE,     /* descriptor table full              */
	MERR_LOOP,
	MERR_NAMETOOLONG,
	MERR_SPIPE,
	MERR_XDEV,
	MERR_NOTSOCK,
	MERR_ADDRINUSE,
	MERR_CONNREFUSED,
	MERR_NOTCONN,
	MERR_AFNOSUPPORT,
	MERR_PROTONOSUPPORT,
	MERR_NOBUFS,
	MERR_MSGSIZE,
	MERR_ISCONN,
	MERR_NXIO,
	MERR_OVERFLOW,
	MERR_NOSPC,
	MERR_ROFS,
	MERR_CONNRESET,
	MERR_DESTADDRREQ,
	MERR_PROTOTYPE,
	MERR_NODEV,
	MERR_BUSY,
	MERR_DEADLK,
	MERR_FBIG,
	MERR_ADDRNOTAVAIL,
	MERR_INPROGRESS,
	MERR_ALREADY,
	MERR_NOPROTOOPT,
	MERR_CONNABORTED,
	MERR_SHUTDOWN,
	MERR_MLINK,
	MERR_NFILE,
	MERR_MAX
};

#endif /* MERGEDOS_ERRNO_H */
