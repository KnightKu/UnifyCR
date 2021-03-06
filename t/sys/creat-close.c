/*
 * Copyright (c) 2018, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2018, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyCR.
 * For details, see https://github.com/LLNL/UnifyCR.
 * Please read https://github.com/LLNL/UnifyCR/LICENSE for full license text.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "t/lib/tap.h"
#include "t/lib/testutil.h"

/* This function contains the tests for UNIFYCR_WRAP(creat) and
 * UNIFYCR_WRAP(close) found in client/src/unifycr-sysio.c.
 *
 * Notice the tests are ordered in a logical testing order. Changing the order
 * or adding new tests in between two others could negatively affect the
 * desired results. */
int creat_close_test(char* unifycr_root)
{
    /* Diagnostic message for reading and debugging output */
    diag("Starting UNIFYCR_WRAP(creat/close) tests");

    char path[64];
    int mode = 0600;
    int fd = -1;
    int rc = -1;

    /* Create a random file name at the mountpoint path to test on */
    testutil_rand_path(path, sizeof(path), unifycr_root);

    /* Verify closing a non-existent file fails with errno=EBADF */
    errno = 0;
    rc = close(fd);
    ok(rc < 0 && errno == EBADF,
       "close non-existing file %s should fail (rc=%d, errno=%d): %s",
       path, rc, errno, strerror(errno));

    /* Verify we can create a non-existent file. */
    errno = 0;
    fd = creat(path, mode);
    ok(fd >= 0, "creat non-existing file %s (fd=%d): %s",
       path, fd, strerror(errno));

    /* Verify close succeeds. */
    errno = 0;
    rc = close(fd);
    ok(rc == 0, "close new file %s (rc=%d): %s", path, rc, strerror(errno));

    /* Verify creating an already created file succeeds. */
    errno = 0;
    fd = creat(path, mode);
    ok(fd >= 0, "creat existing file %s (fd=%d): %s",
       path, fd, strerror(errno));

    /* Verify close succeeds. */
    errno = 0;
    rc = close(fd);
    ok(rc == 0, "close %s (rc=%d): %s", path, rc, strerror(errno));

    /* Verify closing already closed file fails with errno=EBADF */
    errno = 0;
    rc = close(fd);
    ok(rc < 0 && errno == EBADF,
       "close already closed file %s should fail (rc=%d, errno=%d): %s",
       path, rc, errno, strerror(errno));

    /* CLEANUP
     *
     * Don't delete the file at path as the final test (9020-mountpoint-empty)
     * checks if anything files are found in the mountpoint, meaning creat
     * wasn't wrapped properly. */

    diag("Finished UNIFYCR_WRAP(creat/close) tests");

    return 0;
}
