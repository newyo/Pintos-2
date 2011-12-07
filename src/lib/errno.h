#ifndef ERRNO_H__
#define ERRNO_H__

#ifdef _ERRNO_HAS_POSIX_SEMANTICS
// http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/errno.h.html
// ^\[([A-Z]+)\]\n {4}(.+)  -->  \1, // \2
enum
{
  ESUCCESS = 0,
  
  E2BIG, // Argument list too long.
  EACCES, // Permission denied.
  EADDRINUSE, // Address in use.
  EADDRNOTAVAIL, // Address not available.
  EAFNOSUPPORT, // Address family not supported.
  EAGAIN, // Resource unavailable, try again (may be the same value as [EWOULDBLOCK]).
  EALREADY, // Connection already in progress.
  EBADF, // Bad file descriptor.
  EBADMSG, // Bad message.
  EBUSY, // Device or resource busy.
  ECANCELED, // Operation canceled.
  ECHILD, // No child processes.
  ECONNABORTED, // Connection aborted.
  ECONNREFUSED, // Connection refused.
  ECONNRESET, // Connection reset.
  EDEADLK, // Resource deadlock would occur.
  EDESTADDRREQ, // Destination address required.
  EDOM, // Mathematics argument out of domain of function.
  EDQUOT, // Reserved.
  EEXIST, // File exists.
  EFAULT, // Bad address.
  EFBIG, // File too large.
  EHOSTUNREACH, // Host is unreachable.
  EIDRM, // Identifier removed.
  EILSEQ, // Illegal byte sequence.
  EINPROGRESS, // Operation in progress.
  EINTR, // Interrupted function.
  EINVAL, // Invalid argument.
  EIO, // I/O error.
  EISCONN, // Socket is connected.
  EISDIR, // Is a directory.
  ELOOP, // Too many levels of symbolic links.
  EMFILE, // File descriptor value too large.
  EMLINK, // Too many links.
  EMSGSIZE, // Message too large.
  EMULTIHOP, // Reserved.
  ENAMETOOLONG, // Filename too long.
  ENETDOWN, // Network is down.
  ENETRESET, // Connection aborted by network.
  ENETUNREACH, // Network unreachable.
  ENFILE, // Too many files open in system.
  ENOBUFS, // No buffer space available.
  ENODATA, // [OB XSR] [Option Start] No message is available on the STREAM head read queue. [Option End]
  ENODEV, // No such device.
  ENOENT, // No such file or directory.
  ENOEXEC, // Executable file format error.
  ENOLCK, // No locks available.
  ENOLINK, // Reserved.
  ENOMEM, // Not enough space.
  ENOMSG, // No message of the desired type.
  ENOPROTOOPT, // Protocol not available.
  ENOSPC, // No space left on device.
  ENOSR, // [OB XSR] [Option Start] No STREAM resources. [Option End]
  ENOSTR, // [OB XSR] [Option Start] Not a STREAM. [Option End]
  ENOSYS, // Function not supported.
  ENOTCONN, // The socket is not connected.
  ENOTDIR, // Not a directory.
  ENOTEMPTY, // Directory not empty.
  ENOTRECOVERABLE, // State not recoverable.
  ENOTSOCK, // Not a socket.
  ENOTSUP, // Not supported (may be the same value as [EOPNOTSUPP]).
  ENOTTY, // Inappropriate I/O control operation.
  ENXIO, // No such device or address.
  EOVERFLOW, // Value too large to be stored in data type.
  EOWNERDEAD, // Previous owner died.
  EPERM, // Operation not permitted.
  EPIPE, // Broken pipe.
  EPROTO, // Protocol error.
  EPROTONOSUPPORT, // Protocol not supported.
  EPROTOTYPE, // Protocol wrong type for socket.
  ERANGE, // Result too large.
  EROFS, // Read-only file system.
  ESPIPE, // Invalid seek.
  ESRCH, // No such process.
  ESTALE, // Reserved.
  ETIME, // [OB XSR] [Option Start] Stream ioctl() timeout. [Option End]
  ETIMEDOUT, // Connection timed out.
  ETXTBSY, // Text file busy.
  EXDEV // Cross-device link.
};
#else

enum
{
  ESUCCESS = 0,
};

# define E2BIG 1
# define EACCES 1
# define EADDRINUSE 1
# define EADDRNOTAVAIL 1
# define EAFNOSUPPORT 1
# define EAGAIN 1
# define EALREADY 1
# define EBADF 1
# define EBADMSG 1
# define EBUSY 1
# define ECANCELED 1
# define ECHILD 1
# define ECONNABORTED 1
# define ECONNREFUSED 1
# define ECONNRESET 1
# define EDEADLK 1
# define EDESTADDRREQ 1
# define EDOM 1
# define EDQUOT 1
# define EEXIST 1
# define EFAULT 1
# define EFBIG 1
# define EHOSTUNREACH 1
# define EIDRM 1
# define EILSEQ 1
# define EINPROGRESS 1
# define EINTR 1
# define EINVAL 1
# define EIO 1
# define EISCONN 1
# define EISDIR 1
# define ELOOP 1
# define EMFILE 1
# define EMLINK 1
# define EMSGSIZE 1
# define EMULTIHOP 1
# define ENAMETOOLONG 1
# define ENETDOWN 1
# define ENETRESET 1
# define ENETUNREACH 1
# define ENFILE 1
# define ENOBUFS 1
# define ENODATA 1
# define ENODEV 1
# define ENOENT 1
# define ENOEXEC 1
# define ENOLCK 1
# define ENOLINK 1
# define ENOMEM 1
# define ENOMSG 1
# define ENOPROTOOPT 1
# define ENOSPC 1
# define ENOSR 1
# define ENOSTR 1
# define ENOSYS 1
# define ENOTCONN 1
# define ENOTDIR 1
# define ENOTEMPTY 1
# define ENOTRECOVERABLE 1
# define ENOTSOCK 1
# define ENOTSUP 1
# define ENOTTY 1
# define ENXIO 1
# define EOVERFLOW 1
# define EOWNERDEAD 1
# define EPERM 1
# define EPIPE 1
# define EPROTO 1
# define EPROTONOSUPPORT 1
# define EPROTOTYPE 1
# define ERANGE 1
# define EROFS 1
# define ESPIPE 1
# define ESRCH 1
# define ESTALE 1
# define ETIME 1
# define ETIMEDOUT 1
# define ETXTBSY 1
# define EXDEV 1
#endif

#define EOPNOTSUPP ENOTSUP /* Operation not supported on socket (may be the same value as [ENOTSUP]). */
#define EWOULDBLOCK EAGAIN /* Operation would block (may be the same value as [EAGAIN]). */

#endif // ERRNO_H__
