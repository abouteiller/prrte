/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <signal.h>
#ifdef HAVE_UTIL_H
#include <util.h>
#endif
#ifdef HAVE_PTY_H
#include <pty.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
# ifdef HAVE_TERMIO_H
#  include <termio.h>
# endif
#endif
#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#include "src/util/prrte_pty.h"
#include "src/util/prrte_environ.h"
#include "src/util/os_dirpath.h"
#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/base/iof_base_setup.h"

int
prrte_iof_base_setup_prefork(prrte_iof_base_io_conf_t *opts)
{
    int ret = -1;

    fflush(stdout);

    /* first check to make sure we can do ptys */
#if PRRTE_ENABLE_PTY_SUPPORT
    if (opts->usepty) {
        /**
         * It has been reported that on MAC OS X 10.4 and prior one cannot
         * safely close the writing side of a pty before completly reading
         * all data inside.
         * There seems to be two issues: first all pending data is
         * discarded, and second it randomly generate kernel panics.
         * Apparently this issue was fixed in 10.5 so by now we use the
         * pty exactly as we use the pipes.
         * This comment is here as a reminder.
         */
        ret = prrte_openpty(&(opts->p_stdout[0]), &(opts->p_stdout[1]),
                           (char*)NULL, (struct termios*)NULL, (struct winsize*)NULL);
    }
#else
    opts->usepty = 0;
#endif

    if (ret < 0) {
        opts->usepty = 0;
        if (pipe(opts->p_stdout) < 0) {
            PRRTE_ERROR_LOG(PRRTE_ERR_SYS_LIMITS_PIPES);
            return PRRTE_ERR_SYS_LIMITS_PIPES;
        }
    }
    if (opts->connect_stdin) {
        if (pipe(opts->p_stdin) < 0) {
            PRRTE_ERROR_LOG(PRRTE_ERR_SYS_LIMITS_PIPES);
            return PRRTE_ERR_SYS_LIMITS_PIPES;
        }
    }
    if( !prrte_iof_base.redirect_app_stderr_to_stdout ) {
        if (pipe(opts->p_stderr) < 0) {
            PRRTE_ERROR_LOG(PRRTE_ERR_SYS_LIMITS_PIPES);
            return PRRTE_ERR_SYS_LIMITS_PIPES;
        }
    }
    return PRRTE_SUCCESS;
}


int
prrte_iof_base_setup_child(prrte_iof_base_io_conf_t *opts, char ***env)
{
    int ret;

    if (opts->connect_stdin) {
        close(opts->p_stdin[1]);
    }
    close(opts->p_stdout[0]);
    if( !prrte_iof_base.redirect_app_stderr_to_stdout ) {
        close(opts->p_stderr[0]);
    }

    if (opts->usepty) {
        /* disable echo */
        struct termios term_attrs;
        if (tcgetattr(opts->p_stdout[1], &term_attrs) < 0) {
            return PRRTE_ERR_PIPE_SETUP_FAILURE;
        }
        term_attrs.c_lflag &= ~ (ECHO | ECHOE | ECHOK |
                                 ECHOCTL | ECHOKE | ECHONL);
        term_attrs.c_iflag &= ~ (ICRNL | INLCR | ISTRIP | INPCK | IXON);
        term_attrs.c_oflag &= ~ (
#ifdef OCRNL
                                 /* OS X 10.3 does not have this
                                    value defined */
                                 OCRNL |
#endif
                                 ONLCR);
        if (tcsetattr(opts->p_stdout[1], TCSANOW, &term_attrs) == -1) {
            return PRRTE_ERR_PIPE_SETUP_FAILURE;
        }
        ret = dup2(opts->p_stdout[1], fileno(stdout));
        if (ret < 0) {
            return PRRTE_ERR_PIPE_SETUP_FAILURE;
        }
        if( prrte_iof_base.redirect_app_stderr_to_stdout ) {
            ret = dup2(opts->p_stdout[1], fileno(stderr));
            if (ret < 0) {
                return PRRTE_ERR_PIPE_SETUP_FAILURE;
            }
        }
        close(opts->p_stdout[1]);
    } else {
        if(opts->p_stdout[1] != fileno(stdout)) {
            ret = dup2(opts->p_stdout[1], fileno(stdout));
            if (ret < 0) {
                return PRRTE_ERR_PIPE_SETUP_FAILURE;
            }
            if( prrte_iof_base.redirect_app_stderr_to_stdout ) {
                ret = dup2(opts->p_stdout[1], fileno(stderr));
                if (ret < 0) {
                    return PRRTE_ERR_PIPE_SETUP_FAILURE;
                }
            }
            close(opts->p_stdout[1]);
        }
    }
    if (opts->connect_stdin) {
        if(opts->p_stdin[0] != fileno(stdin)) {
            ret = dup2(opts->p_stdin[0], fileno(stdin));
            if (ret < 0) {
                return PRRTE_ERR_PIPE_SETUP_FAILURE;
            }
            close(opts->p_stdin[0]);
        }
    } else {
        int fd;

        /* connect input to /dev/null */
        fd = open("/dev/null", O_RDONLY, 0);
        if(fd != fileno(stdin)) {
            dup2(fd, fileno(stdin));
            close(fd);
        }
    }

    if(opts->p_stderr[1] != fileno(stderr)) {
        if( !prrte_iof_base.redirect_app_stderr_to_stdout ) {
            ret = dup2(opts->p_stderr[1], fileno(stderr));
            if (ret < 0) return PRRTE_ERR_PIPE_SETUP_FAILURE;
            close(opts->p_stderr[1]);
        }
    }

    return PRRTE_SUCCESS;
}


int
prrte_iof_base_setup_parent(const prrte_process_name_t* name,
                           prrte_iof_base_io_conf_t *opts)
{
    int ret;

    /* connect stdin endpoint */
    if (opts->connect_stdin) {
        /* and connect the pty to stdin */
        ret = prrte_iof.pull(name, PRRTE_IOF_STDIN, opts->p_stdin[1]);
        if(PRRTE_SUCCESS != ret) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    /* connect read ends to IOF */
    ret = prrte_iof.push(name, PRRTE_IOF_STDOUT, opts->p_stdout[0]);
    if(PRRTE_SUCCESS != ret) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }

    if( !prrte_iof_base.redirect_app_stderr_to_stdout ) {
        ret = prrte_iof.push(name, PRRTE_IOF_STDERR, opts->p_stderr[0]);
        if(PRRTE_SUCCESS != ret) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_iof_base_setup_output_files(const prrte_process_name_t* dst_name,
                                     prrte_job_t *jobdat,
                                     prrte_iof_proc_t *proct)
{
    int rc;
    char *dirname, *outdir, *outfile;
    int np, numdigs, fdout, i;
    char *p, **s;
    bool usejobid = true;

    /* see if we are to output to a file */
    dirname = NULL;
    if (prrte_get_attribute(&jobdat->attributes, PRRTE_JOB_OUTPUT_TO_FILE, (void**)&dirname, PRRTE_STRING) &&
        NULL != dirname) {
        np = jobdat->num_procs / 10;
        /* determine the number of digits required for max vpid */
        numdigs = 1;
        while (np > 0) {
            numdigs++;
            np = np / 10;
        }
        /* check for a conditional in the directory name */
        if (NULL != (p = strchr(dirname, ':'))) {
            *p = '\0';
            ++p;
            /* could me more than one directive */
            s = prrte_argv_split(p, ',');
            for (i=0; NULL != s[i]; i++) {
                if (0 == strcasecmp(s[i], "nojobid")) {
                    usejobid = false;
                } else if (0 == strcasecmp(s[i], "nocopy")) {
                    proct->copy = false;
                }
            }
        }

        /* construct the directory where the output files will go */
        if (usejobid) {
            prrte_asprintf(&outdir, "%s/%d/rank.%0*lu", dirname,
                     (int)PRRTE_LOCAL_JOBID(proct->name.jobid),
                     numdigs, (unsigned long)proct->name.vpid);
        } else {
            prrte_asprintf(&outdir, "%s/rank.%0*lu", dirname,
                     numdigs, (unsigned long)proct->name.vpid);
        }
        /* ensure the directory exists */
        if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(outdir, S_IRWXU|S_IRGRP|S_IXGRP))) {
            PRRTE_ERROR_LOG(rc);
            free(outdir);
            return rc;
        }
        if (NULL != proct->revstdout && NULL == proct->revstdout->sink) {
            /* setup the stdout sink */
            prrte_asprintf(&outfile, "%s/stdout", outdir);
            fdout = open(outfile, O_CREAT|O_RDWR|O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PRRTE_ERROR_LOG(PRRTE_ERR_FILE_OPEN_FAILURE);
                return PRRTE_ERR_FILE_OPEN_FAILURE;
            }
            /* define a sink to that file descriptor */
            PRRTE_IOF_SINK_DEFINE(&proct->revstdout->sink, dst_name,
                                 fdout, PRRTE_IOF_STDOUT,
                                 prrte_iof_base_write_handler);
        }

        if (NULL != proct->revstderr && NULL == proct->revstderr->sink) {
            /* if they asked for stderr to be combined with stdout, then we
             * only create one file and tell the IOF to put both streams
             * into it. Otherwise, we create separate files for each stream */
            if (prrte_get_attribute(&jobdat->attributes, PRRTE_JOB_MERGE_STDERR_STDOUT, NULL, PRRTE_BOOL)) {
                /* just use the stdout sink */
                PRRTE_RETAIN(proct->revstdout->sink);
                proct->revstdout->sink->tag = PRRTE_IOF_STDMERGE;  // show that it is merged
                proct->revstderr->sink = proct->revstdout->sink;
            } else {
                prrte_asprintf(&outfile, "%s/stderr", outdir);
                fdout = open(outfile, O_CREAT|O_RDWR|O_TRUNC, 0644);
                free(outfile);
                if (fdout < 0) {
                    /* couldn't be opened */
                    PRRTE_ERROR_LOG(PRRTE_ERR_FILE_OPEN_FAILURE);
                    return PRRTE_ERR_FILE_OPEN_FAILURE;
                }
                /* define a sink to that file descriptor */
                PRRTE_IOF_SINK_DEFINE(&proct->revstderr->sink, dst_name,
                                     fdout, PRRTE_IOF_STDERR,
                                     prrte_iof_base_write_handler);
            }
        }
    }

    return PRRTE_SUCCESS;
}