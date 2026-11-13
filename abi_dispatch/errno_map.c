/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Native error -> foreign error translation.
 *
 * Linux and the BSD family agree on the low errno numbers but diverge
 * above 34 (EAGAIN is 11 on Linux and 35 on FreeBSD/macOS; ENOSYS is 38 vs
 * 78). NT does not use errno at all. A binary must always see the numbers
 * its own libc was compiled against.
 */
#include "abi_dispatch/abi.h"

static const u8 linux_errno[MERR_MAX] = {
	[MERR_PERM]   = 1,      /* EPERM       */
	[MERR_NOENT]  = 2,      /* ENOENT      */
	[MERR_IO]     = 5,      /* EIO         */
	[MERR_BADF]   = 9,      /* EBADF       */
	[MERR_NOMEM]  = 12,     /* ENOMEM      */
	[MERR_ACCES]  = 13,     /* EACCES      */
	[MERR_FAULT]  = 14,     /* EFAULT      */
	[MERR_INVAL]  = 22,     /* EINVAL      */
	[MERR_NOEXEC] = 8,      /* ENOEXEC     */
	[MERR_NOSYS]  = 38,     /* ENOSYS      */
	[MERR_NOTSUP] = 95,     /* EOPNOTSUPP  */
	[MERR_AGAIN]  = 11,     /* EAGAIN      */
	[MERR_RANGE]  = 7,      /* E2BIG       */
	[MERR_NOTTY]  = 25,     /* ENOTTY      */
	[MERR_NOTDIR] = 20,     /* ENOTDIR     */
	[MERR_ISDIR]  = 21,     /* EISDIR      */
	[MERR_EXIST]  = 17,     /* EEXIST      */
	[MERR_NOTEMPTY] = 39,   /* ENOTEMPTY   */
	[MERR_CHILD]  = 10,     /* ECHILD      */
	[MERR_INTR]   = 4,      /* EINTR       */
	[MERR_PIPE]   = 32,     /* EPIPE       */
	[MERR_SRCH]   = 3,      /* ESRCH       */
	[MERR_TIMEDOUT] = 110,  /* ETIMEDOUT   */
	[MERR_MFILE]  = 24,     /* EMFILE      */
};

static const u8 bsd_errno[MERR_MAX] = {
	[MERR_PERM]   = 1,
	[MERR_NOENT]  = 2,
	[MERR_IO]     = 5,
	[MERR_BADF]   = 9,
	[MERR_NOMEM]  = 12,
	[MERR_ACCES]  = 13,
	[MERR_FAULT]  = 14,
	[MERR_INVAL]  = 22,
	[MERR_NOEXEC] = 8,
	[MERR_NOSYS]  = 78,     /* ENOSYS      */
	[MERR_NOTSUP] = 45,     /* EOPNOTSUPP / ENOTSUP */
	[MERR_AGAIN]  = 35,     /* EAGAIN      */
	[MERR_RANGE]  = 7,
	[MERR_NOTTY]  = 25,
	[MERR_NOTDIR] = 20,
	[MERR_ISDIR]  = 21,
	[MERR_EXIST]  = 17,
	[MERR_NOTEMPTY] = 66,   /* ENOTEMPTY   */
	[MERR_CHILD]  = 10,
	[MERR_INTR]   = 4,
	[MERR_PIPE]   = 32,
	[MERR_SRCH]   = 3,
	[MERR_TIMEDOUT] = 60,   /* ETIMEDOUT   */
	[MERR_MFILE]  = 24,
};

static const u32 nt_status[MERR_MAX] = {
	[MERR_PERM]   = NT_STATUS_ACCESS_DENIED,
	[MERR_NOENT]  = NT_STATUS_OBJECT_NAME_NOT_FOUND,
	[MERR_IO]     = NT_STATUS_IO_DEVICE_ERROR,
	[MERR_BADF]   = NT_STATUS_INVALID_HANDLE,
	[MERR_NOMEM]  = NT_STATUS_NO_MEMORY,
	[MERR_ACCES]  = NT_STATUS_ACCESS_DENIED,
	[MERR_FAULT]  = NT_STATUS_ACCESS_VIOLATION,
	[MERR_INVAL]  = NT_STATUS_INVALID_PARAMETER,
	[MERR_NOEXEC] = NT_STATUS_INVALID_IMAGE_FORMAT,
	[MERR_NOSYS]  = NT_STATUS_INVALID_SYSTEM_SERVICE,
	[MERR_NOTSUP] = NT_STATUS_NOT_SUPPORTED,
	[MERR_AGAIN]  = NT_STATUS_RETRY,
	[MERR_RANGE]  = NT_STATUS_BUFFER_OVERFLOW,
	[MERR_NOTTY]  = NT_STATUS_INVALID_PARAMETER,
	[MERR_NOTDIR] = 0xC0000103u,            /* STATUS_NOT_A_DIRECTORY       */
	[MERR_ISDIR]  = 0xC00000BAu,            /* STATUS_FILE_IS_A_DIRECTORY   */
	[MERR_EXIST]  = 0xC0000035u,            /* STATUS_OBJECT_NAME_COLLISION */
	[MERR_NOTEMPTY] = 0xC0000101u,          /* STATUS_DIRECTORY_NOT_EMPTY   */
	[MERR_PIPE]   = 0xC000014Bu,            /* STATUS_PIPE_BROKEN           */
	[MERR_SRCH]   = 0xC000000Bu,            /* STATUS_INVALID_CID           */
	[MERR_TIMEDOUT] = 0x00000102u,          /* STATUS_TIMEOUT               */
	[MERR_MFILE]  = 0xC000011Fu,            /* STATUS_TOO_MANY_OPENED_FILES */
};

int abi_errno_linux(int merr)
{
	if (merr <= 0 || merr >= MERR_MAX || !linux_errno[merr])
		return 5;                               /* EIO */
	return linux_errno[merr];
}

int abi_errno_bsd(int merr)
{
	if (merr <= 0 || merr >= MERR_MAX || !bsd_errno[merr])
		return 5;
	return bsd_errno[merr];
}

u32 abi_ntstatus(int merr)
{
	if (merr == 0)
		return NT_STATUS_SUCCESS;
	if (merr < 0 || merr >= MERR_MAX || !nt_status[merr])
		return NT_STATUS_UNSUCCESSFUL;
	return nt_status[merr];
}
