#ifndef __LIBMAIX_ERR_H
#define __LIBMAIX_ERR_H

/* Vendored from sipeed/libmaix (components/libmaix/include/libmaix_err.h).
 * Only the success value (LIBMAIX_ERR_NONE == 0) is relied upon. */
typedef enum
{
    LIBMAIX_ERR_NONE          = 0,
    LIBMAIX_ERR_PARAM         = 1,
    LIBMAIX_ERR_NO_MEM        = 2,
    LIBMAIX_ERR_NOT_IMPLEMENT = 3,
    LIBMAIX_ERR_NOT_READY     = 4,
    LIBMAIX_ERR_NOT_INIT      = 5,
    LIBMAIX_ERR_NOT_PERMIT    = 6,
    LIBMAIX_ERR_NOT_EXEC      = 7,
    LIBMAIX_ERR_UNKNOWN,
} libmaix_err_t;

#endif
