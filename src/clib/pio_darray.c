/** @file
 *
 * Public functions that read and write distributed arrays in PIO.
 *
 * When arrays are distributed, each processor holds some of the
 * array. Only by combining the distributed arrays from all processor
 * can the full array be obtained.
 *
 * @author Jim Edwards
 */
#include <config.h>
#include <pio.h>
#include <pio_internal.h>
#ifdef PIO_MICRO_TIMING
#include "pio_timer.h"
#endif

/* 10MB default limit. */
PIO_Offset pio_buffer_size_limit = 10485760;

/* Global buffer pool pointer. */
void *CN_bpool = NULL;

/* Maximum buffer usage. */
PIO_Offset maxusage = 0;

/* For write_darray_multi_serial() and write_darray_multi_par() to
 * indicate whether fill or data are being written. */
#define DARRAY_FILL 1
#define DARRAY_DATA 0

/**
 * Set the PIO IO node data buffer size limit.
 *
 * The pio_buffer_size_limit will only apply to files opened after
 * the setting is changed.
 *
 * @param limit the size of the buffer on the IO nodes
 * @return The previous limit setting.
 */
PIO_Offset PIOc_set_buffer_size_limit(PIO_Offset limit)
{
    PIO_Offset oldsize = pio_buffer_size_limit;

    /* If the user passed a valid size, use it. */
    if (limit > 0)
        pio_buffer_size_limit = limit;

    return oldsize;
}

/**
 * Write one or more arrays with the same IO decomposition to the
 * file.
 *
 * This funciton is similar to PIOc_write_darray(), but allows the
 * caller to use their own data buffering (instead of using the
 * buffering implemented in PIOc_write_darray()).
 *
 * When the user calls PIOc_write_darray() one or more times, then
 * PIO_write_darray_multi() will be called when the buffer is flushed.
 *
 * Internally, this function will:
 * <ul>
 * <li>Find info about file, decomposition, and variable.
 * <li>Do a special flush for pnetcdf if needed.
 * <li>Allocates a buffer big enough to hold all the data in the
 * multi-buffer, for all tasks.
 * <li>Calls rearrange_comp2io() to move data from compute to IO
 * tasks.
 * <li>For parallel iotypes (pnetcdf and netCDF-4 parallel) call
 * pio_write_darray_multi_nc().
 * <li>For serial iotypes (netcdf classic and netCDF-4 serial) call
 * write_darray_multi_serial().
 * <li>For subset rearranger, create holegrid to write missing
 * data. Then call pio_write_darray_multi_nc() or
 * write_darray_multi_serial() to write the holegrid.
 * <li>Special buffer flush for pnetcdf.
 * </ul>
 *
 * @param ncid identifies the netCDF file.
 * @param varids an array of length nvars containing the variable ids to
 * be written.
 * @param ioid the I/O description ID as passed back by
 * PIOc_InitDecomp().
 * @param nvars the number of variables to be written with this
 * call.
 * @param arraylen the length of the array to be written. This is the
 * length of the distrubited array. That is, the length of the portion
 * of the data that is on the processor. The same arraylen is used for
 * all variables in the call.
 * @param array pointer to the data to be written. This is a pointer
 * to an array of arrays with the distributed portion of the array
 * that is on this processor. There are nvars arrays of data, and each
 * array of data contains one record worth of data for that variable.
 * @param frame an array of length nvars with the frame or record
 * dimension for each of the nvars variables in IOBUF. NULL if this
 * iodesc contains non-record vars.
 * @param fillvalue pointer an array (of length nvars) of pointers to
 * the fill value to be used for missing data.
 * @param flushtodisk non-zero to cause buffers to be flushed to disk.
 * @return 0 for success, error code otherwise.
 * @ingroup PIO_write_darray
 * @author Jim Edwards, Ed Hartnett
 */
int PIOc_write_darray_multi(int ncid, const int *varids, int ioid, int nvars,
                            PIO_Offset arraylen, void *array, const int *frame,
                            void **fillvalue, bool flushtodisk)
{
    iosystem_desc_t *ios;  /* Pointer to io system information. */
    file_desc_t *file;     /* Pointer to file information. */
    io_desc_t *iodesc;     /* Pointer to IO description information. */
    int rlen;              /* Total data buffer size. */
    var_desc_t *vdesc0;    /* Array of var_desc structure for each var. */
    int fndims;            /* Number of dims in the var in the file. */
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function calls. */
    int ierr;              /* Return code. */

#ifdef TIMING
    GPTLstart("PIO:PIOc_write_darray_multi");
#endif
    /* Get the file info. */
    if ((ierr = pio_get_file(ncid, &file)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);
    ios = file->iosystem;

    /* Check inputs. */
    if (nvars <= 0 || !varids)
        return pio_err(ios, file, PIO_EINVAL, __FILE__, __LINE__);
    for (int v = 0; v < nvars; v++)
        if (varids[v] < 0 || varids[v] > PIO_MAX_VARS)
            return pio_err(ios, file, PIO_EINVAL, __FILE__, __LINE__);

    LOG((1, "PIOc_write_darray_multi ncid = %d ioid = %d nvars = %d arraylen = %ld "
         "flushtodisk = %d",
         ncid, ioid, nvars, arraylen, flushtodisk));

    /* Check that we can write to this file. */
    if (!(file->mode & PIO_WRITE))
        return pio_err(ios, file, PIO_EPERM, __FILE__, __LINE__);

    /* Get iodesc. */
    if (!(iodesc = pio_get_iodesc_from_id(ioid)))
        return pio_err(ios, file, PIO_EBADID, __FILE__, __LINE__);
    pioassert(iodesc->rearranger == PIO_REARR_BOX || iodesc->rearranger == PIO_REARR_SUBSET,
              "unknown rearranger", __FILE__, __LINE__);

    /* Get a pointer to the variable info for the first variable. */
    vdesc0 = &file->varlist[varids[0]];

    /* Run these on all tasks if async is not in use, but only on
     * non-IO tasks if async is in use. */
    if (!ios->async || !ios->ioproc)
    {
        /* Get the number of dims for this var. */
        LOG((3, "about to call PIOc_inq_varndims varids[0] = %d", varids[0]));
        if ((ierr = PIOc_inq_varndims(file->pio_ncid, varids[0], &fndims)))
            return check_netcdf(file, ierr, __FILE__, __LINE__);
        LOG((3, "called PIOc_inq_varndims varids[0] = %d fndims = %d", varids[0], fndims));
    }

    /* If async is in use, and this is not an IO task, bcast the parameters. */
    if (ios->async)
    {
        if (!ios->ioproc)
        {
            int msg = PIO_MSG_WRITEDARRAYMULTI;
            char frame_present = frame ? true : false;         /* Is frame non-NULL? */
            char fillvalue_present = fillvalue ? true : false; /* Is fillvalue non-NULL? */
            int flushtodisk_int = flushtodisk; /* Need this to be int not boolean. */

            if (ios->compmaster == MPI_ROOT)
                mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);

            /* Send the function parameters and associated informaiton
             * to the msg handler. */
            if (!mpierr)
                mpierr = MPI_Bcast(&ncid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&nvars, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast((void *)varids, nvars, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&ioid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&arraylen, 1, MPI_OFFSET, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(array, arraylen * iodesc->piotype_size, MPI_CHAR, ios->compmaster,
                                   ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&frame_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (!mpierr && frame_present)
                mpierr = MPI_Bcast((void *)frame, nvars, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&fillvalue_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (!mpierr && fillvalue_present)
                mpierr = MPI_Bcast((void *)fillvalue, nvars * iodesc->piotype_size, MPI_CHAR,
                                   ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&flushtodisk_int, 1, MPI_INT, ios->compmaster, ios->intercomm);
            LOG((2, "PIOc_write_darray_multi file->pio_ncid = %d nvars = %d ioid = %d arraylen = %d "
                 "frame_present = %d fillvalue_present = %d flushtodisk = %d", file->pio_ncid, nvars,
                 ioid, arraylen, frame_present, fillvalue_present, flushtodisk));
        }

        /* Handle MPI errors. */
        if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
            return check_mpi(file, mpierr2, __FILE__, __LINE__);
        if (mpierr)
            return check_mpi(file, mpierr, __FILE__, __LINE__);

        /* Share results known only on computation tasks with IO tasks. */
        if ((mpierr = MPI_Bcast(&fndims, 1, MPI_INT, ios->comproot, ios->my_comm)))
            check_mpi(file, mpierr, __FILE__, __LINE__);
        LOG((3, "shared fndims = %d", fndims));
    }

    /* if the buffer is already in use in pnetcdf we need to flush first */
    if (file->iotype == PIO_IOTYPE_PNETCDF && file->iobuf)
	flush_output_buffer(file, 1, 0);

    pioassert(!file->iobuf, "buffer overwrite",__FILE__, __LINE__);

    /* Determine total size of aggregated data (all vars/records).
     * For netcdf serial writes we collect the data on io nodes and
     * then move that data one node at a time to the io master node
     * and write (or read). The buffer size on io task 0 must be as
     * large as the largest used to accommodate this serial io
     * method.  */
    rlen = iodesc->maxiobuflen * nvars;

#ifdef PIO_MICRO_TIMING
    bool var_mtimer_was_running[nvars];
    /* Use the timer on the first variable to capture the total
      *time to rearrange data for all variables
      */
    ierr = mtimer_start(file->varlist[varids[0]].wr_rearr_mtimer);
    if(ierr != PIO_NOERR)
    {
        LOG((1, "ERROR: Unable to start wr rearr timer"));
        return pio_err(ios, file, ierr, __FILE__, __LINE__);
    }
    /* Stop any write timers that are running, these timers will
      *be updated later with the avg rearrange time 
      * (wr_rearr_mtimer)
      */
    for(int i=0; i<nvars; i++)
    {
        var_mtimer_was_running[i] = false;
        assert(mtimer_is_valid(file->varlist[varids[i]].wr_mtimer));
        ierr = mtimer_pause(file->varlist[varids[i]].wr_mtimer,
                &(var_mtimer_was_running[i]));
        if(ierr != PIO_NOERR)
        {
            LOG((1, "ERROR: Unable to pause write timer"));
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        }
    }
#endif

    /* Allocate iobuf. */
    if (rlen > 0)
    {
        /* Allocate memory for the buffer for all vars/records. */
        if (!(file->iobuf = bget(iodesc->mpitype_size * (size_t)rlen)))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);
        LOG((3, "allocated %lld bytes for variable buffer", (size_t)rlen * iodesc->mpitype_size));

        /* If fill values are desired, and we're using the BOX
         * rearranger, insert fill values. */
        if (iodesc->needsfill && iodesc->rearranger == PIO_REARR_BOX)
        {
            LOG((3, "inerting fill values iodesc->maxiobuflen = %d", iodesc->maxiobuflen));
            for (int nv = 0; nv < nvars; nv++)
                for (int i = 0; i < iodesc->maxiobuflen; i++)
                    memcpy(&((char *)file->iobuf)[iodesc->mpitype_size * (i + nv * iodesc->maxiobuflen)],
                           &((char *)fillvalue)[nv * iodesc->mpitype_size], iodesc->mpitype_size);
        }
    }
    else if (file->iotype == PIO_IOTYPE_PNETCDF && ios->ioproc)
    {
	/* this assures that iobuf is allocated on all iotasks thus
	 assuring that the flush_output_buffer call above is called
	 collectively (from all iotasks) */
        if (!(file->iobuf = bget(1)))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);
        LOG((3, "allocated token for variable buffer"));
    }

    /* Move data from compute to IO tasks. */
    if ((ierr = rearrange_comp2io(ios, iodesc, array, file->iobuf, nvars)))
        return pio_err(ios, file, ierr, __FILE__, __LINE__);

#ifdef PIO_MICRO_TIMING
    double rearr_time = 0;
    /* Use the timer on the first variable to capture the total
      *time to rearrange data for all variables
      */
    ierr = mtimer_pause(file->varlist[varids[0]].wr_rearr_mtimer, NULL);
    if(ierr != PIO_NOERR)
    {
        LOG((1, "ERROR: Unable to pause wr rearr timer"));
        return pio_err(ios, file, ierr, __FILE__, __LINE__);
    }

    ierr = mtimer_get_wtime(file->varlist[varids[0]].wr_rearr_mtimer,
            &rearr_time);
    if(ierr != PIO_NOERR)
    {
        LOG((1, "ERROR: Unable to get wtime from wr rearr timer"));
        return pio_err(ios, file, ierr, __FILE__, __LINE__);
    }

    /* Calculate the average rearrange time for a variable */
    rearr_time /= nvars;
    for(int i=0; i<nvars; i++)
    {
        /* Reset, update and flush each timer */
        ierr = mtimer_reset(file->varlist[varids[i]].wr_rearr_mtimer);
        if(ierr != PIO_NOERR)
        {
            LOG((1, "ERROR: Unable to reset wr rearr timer"));
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        }

        /* Update the rearrange timer with avg rearrange time for a var */
        ierr = mtimer_update(file->varlist[varids[i]].wr_rearr_mtimer,
                rearr_time);
        if(ierr != PIO_NOERR)
        {
            LOG((1, "ERROR: Unable to update wr rearr timer"));
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        }
        ierr = mtimer_flush(file->varlist[varids[i]].wr_rearr_mtimer,
                get_var_desc_str(file->pio_ncid, varids[i], NULL));
        if(ierr != PIO_NOERR)
        {
            LOG((1, "ERROR: Unable to flush wr rearr timer"));
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        }
        /* Update the write timer with avg rearrange time for a var
         * i.e, the write timer includes the rearrange time
         */
        ierr = mtimer_update(file->varlist[varids[i]].wr_mtimer,
                rearr_time);
        if(ierr != PIO_NOERR)
        {
            LOG((1, "ERROR: Unable to update wr timer"));
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        }

        /* If the write timer was already running, resume it */
        if(var_mtimer_was_running[i])
        {
            ierr = mtimer_resume(file->varlist[varids[i]].wr_mtimer);
            if(ierr != PIO_NOERR)
            {
                LOG((1, "ERROR: Unable to resume wr timer"));
                return pio_err(ios, file, ierr, __FILE__, __LINE__);
            }
        }
    }
#endif
    /* Write the darray based on the iotype. */
    LOG((2, "about to write darray for iotype = %d", file->iotype));
    switch (file->iotype)
    {
    case PIO_IOTYPE_NETCDF4P:
    case PIO_IOTYPE_PNETCDF:
        if ((ierr = write_darray_multi_par(file, nvars, fndims, varids, iodesc,
                                           DARRAY_DATA, frame)))
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        break;
    case PIO_IOTYPE_NETCDF4C:
    case PIO_IOTYPE_NETCDF:
        if ((ierr = write_darray_multi_serial(file, nvars, fndims, varids, iodesc,
                                              DARRAY_DATA, frame)))
            return pio_err(ios, file, ierr, __FILE__, __LINE__);

        break;
    default:
        return pio_err(NULL, NULL, PIO_EBADIOTYPE, __FILE__, __LINE__);
    }

    /* For PNETCDF the iobuf is freed in flush_output_buffer() */
    if (file->iotype != PIO_IOTYPE_PNETCDF)
    {
        /* Release resources. */
        if (file->iobuf)
        {
	    LOG((3,"freeing variable buffer in pio_darray"));
            brel(file->iobuf);
            file->iobuf = NULL;
        }
    }

    /* The box rearranger will always have data (it could be fill
     * data) to fill the entire array - that is the aggregate start
     * and count values will completely describe one unlimited
     * dimension unit of the array. For the subset method this is not
     * necessarily the case, areas of missing data may never be
     * written. In order to make sure that these areas are given the
     * missing value a 'holegrid' is used to describe the missing
     * points. This is generally faster than the netcdf method of
     * filling the entire array with missing values before overwriting
     * those values later. */
    if (iodesc->rearranger == PIO_REARR_SUBSET && iodesc->needsfill)
    {
        LOG((2, "nvars = %d holegridsize = %ld iodesc->needsfill = %d\n", nvars,
             iodesc->holegridsize, iodesc->needsfill));

	pioassert(!vdesc0->fillbuf, "buffer overwrite",__FILE__, __LINE__);

        /* Get a buffer. */
	if (ios->io_rank == 0)
	    vdesc0->fillbuf = bget(iodesc->maxholegridsize * iodesc->mpitype_size * nvars);
	else if (iodesc->holegridsize > 0)
	    vdesc0->fillbuf = bget(iodesc->holegridsize * iodesc->mpitype_size * nvars);

        /* copying the fill value into the data buffer for the box
         * rearranger. This will be overwritten with data where
         * provided. */
        for (int nv = 0; nv < nvars; nv++)
            for (int i = 0; i < iodesc->holegridsize; i++)
                memcpy(&((char *)vdesc0->fillbuf)[iodesc->mpitype_size * (i + nv * iodesc->holegridsize)],
                       &((char *)fillvalue)[iodesc->mpitype_size * nv], iodesc->mpitype_size);

        /* Write the darray based on the iotype. */
        switch (file->iotype)
        {
        case PIO_IOTYPE_PNETCDF:
        case PIO_IOTYPE_NETCDF4P:
            if ((ierr = write_darray_multi_par(file, nvars, fndims, varids, iodesc,
                                               DARRAY_FILL, frame)))
                return pio_err(ios, file, ierr, __FILE__, __LINE__);
            break;
        case PIO_IOTYPE_NETCDF4C:
        case PIO_IOTYPE_NETCDF:
            if ((ierr = write_darray_multi_serial(file, nvars, fndims, varids, iodesc,
                                                  DARRAY_FILL, frame)))
                return pio_err(ios, file, ierr, __FILE__, __LINE__);
            break;
        default:
            return pio_err(ios, file, PIO_EBADIOTYPE, __FILE__, __LINE__);
        }

        /* For PNETCDF fillbuf is freed in flush_output_buffer() */
        if (file->iotype != PIO_IOTYPE_PNETCDF)
        {
            /* Free resources. */
            if (vdesc0->fillbuf)
            {
                brel(vdesc0->fillbuf);
                vdesc0->fillbuf = NULL;
            }
        }
    }

    /* Only PNETCDF does non-blocking buffered writes, and hence
     * needs an explicit flush/wait to make sure data is written
     * to disk (if the buffer is full)
     */
    if (ios->ioproc && file->iotype == PIO_IOTYPE_PNETCDF)
    {
        /* Flush data to disk for pnetcdf. */
        if ((ierr = flush_output_buffer(file, flushtodisk, 0)))
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
    }
    else
    {
        for(int i=0; i<nvars; i++)
        {
            file->varlist[varids[i]].wb_pend = 0;
#ifdef PIO_MICRO_TIMING
            /* No more async events pending (all buffered data is written out) */
            mtimer_async_event_in_progress(file->varlist[varids[i]].wr_mtimer, false);
            mtimer_flush(file->varlist[varids[i]].wr_mtimer, get_var_desc_str(file->pio_ncid, varids[i], NULL));
#endif
        }
        file->wb_pend = 0;
    }

#ifdef TIMING
    GPTLstop("PIO:PIOc_write_darray_multi");
#endif
    return PIO_NOERR;
}

/**
 * Find the fillvalue that should be used for a variable.
 *
 * @param file Info about file we are writing to. 
 * @param varid the variable ID.
 * @param vdesc pointer to var_desc_t info for this var.
 * @returns 0 for success, non-zero error code for failure.
 * @ingroup PIO_write_darray
 * @author Ed Hartnett 
*/
int find_var_fillvalue(file_desc_t *file, int varid, var_desc_t *vdesc)
{
    iosystem_desc_t *ios;  /* Pointer to io system information. */    
    int no_fill;
    int ierr;

    /* Check inputs. */
    pioassert(file && file->iosystem && vdesc, "invalid input", __FILE__, __LINE__);
    ios = file->iosystem;
    
    LOG((3, "find_var_fillvalue file->pio_ncid = %d varid = %d", file->pio_ncid, varid));
    
    /* Find out PIO data type of var. */
    if ((ierr = PIOc_inq_vartype(file->pio_ncid, varid, &vdesc->pio_type)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    
    /* Find out length of type. */
    if ((ierr = PIOc_inq_type(file->pio_ncid, vdesc->pio_type, NULL, &vdesc->type_size)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    LOG((3, "getting fill value for varid = %d pio_type = %d type_size = %d",
         varid, vdesc->pio_type, vdesc->type_size));
    
    /* Allocate storage for the fill value. */
    if (!(vdesc->fillvalue = malloc(vdesc->type_size)))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    
    /* Get the fill value. */
    if ((ierr = PIOc_inq_var_fill(file->pio_ncid, varid, &no_fill, vdesc->fillvalue)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    vdesc->use_fill = no_fill ? 0 : 1;
    LOG((3, "vdesc->use_fill = %d", vdesc->use_fill));

    return PIO_NOERR;
}

/* Check if the write multi buffer requires a flush
 * wmb : A write multi buffer that might already contain data
 * arraylen : The length of the new array that needs to be cached in this wmb
 *            (The array is not cached yet)
 * iodesc : io descriptor for the data cached in the write multi buffer
 * A disk flush implies that data needs to be rearranged and write needs to be
 * completed. Rearranging and writing data frees up cache is compute and I/O
 * processes
 * An I/O flush implies that data needs to be rearranged and write needs to be
 * started (for iotypes other than PnetCDF write also completes). This would
 * free up cache in compute processes (I/O processes still need to cache the
 * rearranged data until the write completes)
 * Returns 2 if a disk flush is required, 1 if an I/O flush is required, 0 otherwise
 */
static int PIO_wmb_needs_flush(wmulti_buffer *wmb, int arraylen, io_desc_t *iodesc)
{
    bufsize curalloc, totfree, maxfree;
    long nget, nrel;
    const int NEEDS_DISK_FLUSH=2, NEEDS_IO_FLUSH=1, NO_FLUSH=0;

    assert(wmb && iodesc);
    /* Find out how much free, contiguous space is available. */
    bstats(&curalloc, &totfree, &maxfree, &nget, &nrel);

    /* We have exceeded the set buffer write cache limit, write data to
     * disk
     */
    if(curalloc >= pio_buffer_size_limit)
    {
        return NEEDS_DISK_FLUSH;
    }

    PIO_Offset array_sz_bytes = arraylen * iodesc->mpitype_size;
    /* Total cache size required to cache this array
     * - including existing data cached in wmb
     * Note that all the arrays are cached in an wmb in a single
     * contiguous block of memory.
     */
    PIO_Offset wmb_req_cache_sz = (1 + wmb->num_arrays) * array_sz_bytes;
    /* maxfree is the maximum amount of contiguous memory available.
     * if maxfree <= 110% of the current size of wmb cache, it is close
     * to being exhausted/filled, flush so that we have enough space
     * to satisfy future requests
     * FIXME: What is the logic for using 110% here?
     */ 
    if(maxfree <= 1.1 * wmb_req_cache_sz)
    {
        return NEEDS_IO_FLUSH;
    }

    return NO_FLUSH;
}

/**
 * Write a distributed array to the output file.
 *
 * This routine aggregates output on the compute nodes and only sends
 * it to the IO nodes when the compute buffer is full or when a flush
 * is triggered.
 *
 * Internally, this function will:
 * <ul>
 * <li>Locate info about this file, decomposition, and variable.
 * <li>If we don't have a fillvalue for this variable, determine one
 * and remember it for future calls.
 * <li>Initialize or find the multi_buffer for this record/var.
 * <li>Find out how much free space is available in the multi buffer
 * and flush if needed.
 * <li>Store the new user data in the mutli buffer.
 * <li>If needed (only for subset rearranger), fill in gaps in data
 * with fillvalue.
 * <li>Remember the frame value (i.e. record number) of this data if
 * there is one.
 * </ul>
 *
 * NOTE: The write multi buffer wmulti_buffer is the cache on compute
 * nodes that will collect and store multiple variables before sending
 * them to the io nodes. Aggregating variables in this way leads to a
 * considerable savings in communication expense. Variables in the wmb
 * array must have the same decomposition and base data size and we
 * also need to keep track of whether each is a recordvar (has an
 * unlimited dimension) or not.
 *
 * @param ncid the ncid of the open netCDF file.
 * @param varid the ID of the variable that these data will be written
 * to.
 * @param ioid the I/O description ID as passed back by
 * PIOc_InitDecomp().
 * @param arraylen the length of the array to be written. This should
 * be at least the length of the local component of the distrubited
 * array. (Any values beyond length of the local component will be
 * ignored.)
 * @param array pointer to an array of length arraylen with the data
 * to be written. This is a pointer to the distributed portion of the
 * array that is on this task.
 * @param fillvalue pointer to the fill value to be used for missing
 * data.
 * @returns 0 for success, non-zero error code for failure.
 * @ingroup PIO_write_darray
 * @author Jim Edwards, Ed Hartnett
 */
int PIOc_write_darray(int ncid, int varid, int ioid, PIO_Offset arraylen, void *array,
                      void *fillvalue)
{
    iosystem_desc_t *ios;  /* Pointer to io system information. */
    file_desc_t *file;     /* Info about file we are writing to. */
    io_desc_t *iodesc;     /* The IO description. */
    var_desc_t *vdesc;     /* Info about the var being written. */
    void *bufptr;          /* A data buffer. */
    MPI_Datatype vtype;    /* The MPI type of the variable. */
    wmulti_buffer *wmb;    /* The write multi buffer for one or more vars. */
    int recordvar;         /* Non-zero if this is a record variable. */
    int needsflush = 0;    /* True if we need to flush buffer. */
#if PIO_USE_MALLOC
    void *realloc_data = NULL;
#else
    bufsize totfree;       /* Amount of free space in the buffer. */
    bufsize maxfree;       /* Max amount of free space in buffer. */
#endif
    PIO_Offset decomp_max_regions; /* Max non-contiguous regions in the IO decomposition */
    PIO_Offset io_max_regions; /* Max non-contiguous regions cached in a single IO process */
    int mpierr = MPI_SUCCESS;  /* Return code from MPI functions. */
    int ierr = PIO_NOERR;  /* Return code. */

#ifdef TIMING
    GPTLstart("PIO:PIOc_write_darray");
#endif
    LOG((1, "PIOc_write_darray ncid = %d varid = %d ioid = %d arraylen = %d",
         ncid, varid, ioid, arraylen));

    /* Get the file info. */
    if ((ierr = pio_get_file(ncid, &file)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);
    ios = file->iosystem;

    LOG((1, "PIOc_write_darray ncid=%d varid=%d wb_pend=%llu file_wb_pend=%llu",
          ncid, varid,
          (unsigned long long int) file->varlist[varid].wb_pend,
          (unsigned long long int) file->wb_pend
    ));

    /* Can we write to this file? */
    if (!(file->mode & PIO_WRITE))
        return pio_err(ios, file, PIO_EPERM, __FILE__, __LINE__);

    /* Get decomposition information. */
    if (!(iodesc = pio_get_iodesc_from_id(ioid)))
        return pio_err(ios, file, PIO_EBADID, __FILE__, __LINE__);

    /* Check that the local size of the variable passed in matches the
     * size expected by the io descriptor. Fail if arraylen is too
     * small, just put a warning in the log and truncate arraylen
     * if it is too big (the excess values will be ignored.) */
    if (arraylen < iodesc->ndof)
        return pio_err(ios, file, PIO_EINVAL, __FILE__, __LINE__);
    LOG((2, "%s arraylen = %d iodesc->ndof = %d",
         (arraylen > iodesc->ndof) ? "WARNING: arraylen > iodesc->ndof" : "",
         arraylen, iodesc->ndof));
    if (arraylen > iodesc->ndof)
        arraylen = iodesc->ndof;

#ifdef PIO_MICRO_TIMING
    mtimer_start(file->varlist[varid].wr_mtimer);
#endif

    /* Get var description. */
    vdesc = &(file->varlist[varid]);
    LOG((2, "vdesc record %d nreqs %d", vdesc->record, vdesc->nreqs));

    /* If we don't know the fill value for this var, get it. */
    if (!vdesc->fillvalue)
        if ((ierr = find_var_fillvalue(file, varid, vdesc)))
            return pio_err(ios, file, PIO_EBADID, __FILE__, __LINE__);            

    /* Is this a record variable? The user must set the vdesc->record
     * value by calling PIOc_setframe() before calling this
     * function. */
    recordvar = vdesc->record >= 0 ? 1 : 0;
    LOG((3, "recordvar = %d looking for multibuffer", recordvar));

    /* Move to end of list or the entry that matches this ioid. */
    for (wmb = &file->buffer; wmb->next; wmb = wmb->next)
        if (wmb->ioid == ioid && wmb->recordvar == recordvar)
            break;
    LOG((3, "wmb->ioid = %d wmb->recordvar = %d", wmb->ioid, wmb->recordvar));

    /* If we did not find an existing wmb entry, create a new wmb. */
    if (wmb->ioid != ioid || wmb->recordvar != recordvar)
    {
        /* Allocate a buffer. */
        LOG((3, "allocating multi-buffer"));
        if (!(wmb->next = calloc(1, sizeof(wmulti_buffer))))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);
        LOG((3, "allocated multi-buffer"));

        /* Set pointer to newly allocated buffer and initialize.*/
        wmb = wmb->next;
        wmb->recordvar = recordvar;
        wmb->next = NULL;
        wmb->ioid = ioid;
        wmb->num_arrays = 0;
        wmb->arraylen = arraylen;
        wmb->vid = NULL;
        wmb->data = NULL;
        wmb->frame = NULL;
        wmb->fillvalue = NULL;
    }
    LOG((2, "wmb->num_arrays = %d arraylen = %d iodesc->mpitype_size = %d\n",
         wmb->num_arrays, arraylen, iodesc->mpitype_size));

    needsflush = PIO_wmb_needs_flush(wmb, arraylen, iodesc);
    assert(needsflush >= 0);

    /* When using PIO with PnetCDF + SUBSET rearranger the number
       of non-contiguous regions cached in a single IO process can
       grow to a large number. PnetCDF is not efficient at handling
       very large number of regions (sub-array requests) in the
       data written out. We typically run out of memory or the
       write is very slow.

       We need to set a limit on the potential (after rearrangement)
       maximum number of non-contiguous regions in an IO process and
       forcefully flush out user data cached by a compute process
       when that limit has been reached. */
    decomp_max_regions = (iodesc->maxregions >= iodesc->maxfillregions)? iodesc->maxregions : iodesc->maxfillregions;
    io_max_regions = (1 + wmb->num_arrays) * decomp_max_regions;
    if (io_max_regions > PIO_MAX_CACHED_IO_REGIONS)
        needsflush = 2;

    /* Tell all tasks on the computation communicator whether we need
     * to flush data. */
    if ((mpierr = MPI_Allreduce(MPI_IN_PLACE, &needsflush, 1,  MPI_INT,  MPI_MAX,
                                ios->comp_comm)))
        return check_mpi(file, mpierr, __FILE__, __LINE__);
    LOG((2, "needsflush = %d", needsflush));

    if(!ios->async || !ios->ioproc)
    {
        if(file->varlist[varid].vrsize == 0)
        {
            ierr = calc_var_rec_sz(ncid, varid);
            if(ierr != PIO_NOERR)
            {
                LOG((1, "Unable to calculate the variable record size"));
            }
        }
    }
    /* Flush data if needed. */
    if (needsflush > 0)
    {
#if !PIO_USE_MALLOC
#ifdef PIO_ENABLE_LOGGING
        /* Collect a debug report about buffer. */
        cn_buffer_report(ios, true);
        LOG((2, "maxfree = %ld wmb->num_arrays = %d (1 + wmb->num_arrays) *"
             " arraylen * iodesc->mpitype_size = %ld totfree = %ld\n", maxfree, wmb->num_arrays,
             (1 + wmb->num_arrays) * arraylen * iodesc->mpitype_size, totfree));
#endif /* PIO_ENABLE_LOGGING */
#endif /* !PIO_USE_MALLOC */

        /* Flush buffer to I/O processes - rearrange data and
         * start writing data from the I/O processes
         * Note : Setting the last flag in flush_buffer to
         * true will force flush the buffer to disk for all
         * iotypes (wait for write to complete for PnetCDF)
         */
        if ((ierr = flush_buffer(ncid, wmb, (needsflush == 2))))
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
    }

    /* One record size (sum across all procs) of data is buffered */
    file->varlist[varid].wb_pend += file->varlist[varid].vrsize;
    file->wb_pend += file->varlist[varid].vrsize;
    LOG((1, "Current pending bytes for ncid=%d, varid=%d var_wb_pend= %llu, file_wb_pend=%llu",
          ncid, varid,
          (unsigned long long int) file->varlist[varid].wb_pend,
          (unsigned long long int) file->wb_pend
    ));
    /* Buffering data is considered an async event (to indicate
      *that the event is not yet complete)
      */
#ifdef PIO_MICRO_TIMING
    mtimer_async_event_in_progress(file->varlist[varid].wr_mtimer, true);
#endif

    /* Get memory for data. */
    if (arraylen > 0)
    {
        if (!(wmb->data = bgetr(wmb->data, (1 + wmb->num_arrays) * arraylen * iodesc->mpitype_size)))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);
        LOG((2, "got %ld bytes for data", (1 + wmb->num_arrays) * arraylen * iodesc->mpitype_size));
    }

    /* vid is an array of variable ids in the wmb list, grow the list
     * and add the new entry. */
    if (!(wmb->vid = realloc(wmb->vid, sizeof(int) * (1 + wmb->num_arrays))))
        return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);

    /* wmb->frame is the record number, we assume that the variables
     * in the wmb list may not all have the same unlimited dimension
     * value although they usually do. */
    if (vdesc->record >= 0)
        if (!(wmb->frame = realloc(wmb->frame, sizeof(int) * (1 + wmb->num_arrays))))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);

    /* If we need a fill value, get it. If we are using the subset
     * rearranger and not using the netcdf fill mode then we need to
     * do an extra write to fill in the holes with the fill value. */
    if (iodesc->needsfill)
    {
        /* Get memory to hold fill value. */
        if (!(wmb->fillvalue = bgetr(wmb->fillvalue, iodesc->mpitype_size * (1 + wmb->num_arrays))))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);

        /* If the user passed a fill value, use that, otherwise use
         * the default fill value of the netCDF type. Copy the fill
         * value to the buffer. */
        if (fillvalue)
        {
            memcpy((char *)wmb->fillvalue + iodesc->mpitype_size * wmb->num_arrays,
                   fillvalue, iodesc->mpitype_size);
            LOG((3, "copied user-provided fill value iodesc->mpitype_size = %d",
                 iodesc->mpitype_size));
        }
        else
        {
            void *fill;
            signed char byte_fill = PIO_FILL_BYTE;
            char char_fill = PIO_FILL_CHAR;
            short short_fill = PIO_FILL_SHORT;
            int int_fill = PIO_FILL_INT;
            float float_fill = PIO_FILL_FLOAT;
            double double_fill = PIO_FILL_DOUBLE;
#ifdef _NETCDF4
            unsigned char ubyte_fill = PIO_FILL_UBYTE;
            unsigned short ushort_fill = PIO_FILL_USHORT;
            unsigned int uint_fill = PIO_FILL_UINT;
            long long int64_fill = PIO_FILL_INT64;
            long long uint64_fill = PIO_FILL_UINT64;
#endif /* _NETCDF4 */
            vtype = (MPI_Datatype)iodesc->mpitype;
            LOG((3, "caller did not provide fill value vtype = %d", vtype));

            /* This must be done with an if statement, not a case, or
             * openmpi will not build. */
            if (vtype == MPI_BYTE)
                fill = &byte_fill;
            else if (vtype == MPI_CHAR)
                fill = &char_fill;
            else if (vtype == MPI_SHORT)
                fill = &short_fill;
            else if (vtype == MPI_INT)
                fill = &int_fill;
            else if (vtype == MPI_FLOAT)
                fill = &float_fill;
            else if (vtype == MPI_DOUBLE)
                fill = &double_fill;
#ifdef _NETCDF4
            else if (vtype == MPI_UNSIGNED_CHAR)
                fill = &ubyte_fill;
            else if (vtype == MPI_UNSIGNED_SHORT)
                fill = &ushort_fill;
            else if (vtype == MPI_UNSIGNED)
                fill = &uint_fill;
            else if (vtype == MPI_LONG_LONG)
                fill = &int64_fill;
            else if (vtype == MPI_UNSIGNED_LONG_LONG)
                fill = &uint64_fill;
#endif /* _NETCDF4 */
            else
                return pio_err(ios, file, PIO_EBADTYPE, __FILE__, __LINE__);

            memcpy((char *)wmb->fillvalue + iodesc->mpitype_size * wmb->num_arrays,
                   fill, iodesc->mpitype_size);
            LOG((3, "copied fill value"));
        }
    }

    /* Tell the buffer about the data it is getting. */
    wmb->arraylen = arraylen;
    wmb->vid[wmb->num_arrays] = varid;
    LOG((3, "wmb->num_arrays = %d wmb->vid[wmb->num_arrays] = %d", wmb->num_arrays,
         wmb->vid[wmb->num_arrays]));

    /* Copy the user-provided data to the buffer. */
    bufptr = (void *)((char *)wmb->data + arraylen * iodesc->mpitype_size * wmb->num_arrays);
    if (arraylen > 0)
    {
        memcpy(bufptr, array, arraylen * iodesc->mpitype_size);
        LOG((3, "copied %ld bytes of user data", arraylen * iodesc->mpitype_size));
    }

    /* Add the unlimited dimension value of this variable to the frame
     * array in wmb. */
    if (wmb->frame)
        wmb->frame[wmb->num_arrays] = vdesc->record;
    wmb->num_arrays++;

    LOG((2, "wmb->num_arrays = %d iodesc->maxbytes / iodesc->mpitype_size = %d "
         "iodesc->ndof = %d iodesc->llen = %d", wmb->num_arrays,
         iodesc->maxbytes / iodesc->mpitype_size, iodesc->ndof, iodesc->llen));

        LOG((1, "Write darray end : pending bytes for ncid=%d, varid=%d var_wb_pend=%llu file_wb_pend=%llu",
              ncid, varid,
              (unsigned long long int) file->varlist[varid].wb_pend,
              (unsigned long long int) file->wb_pend
        ));
#ifdef PIO_MICRO_TIMING
    mtimer_stop(file->varlist[varid].wr_mtimer, get_var_desc_str(ncid, varid, NULL));
#endif
#ifdef TIMING
    GPTLstop("PIO:PIOc_write_darray");
#endif
    return PIO_NOERR;
}

/**
 * Read a field from a file to the IO library.
 *
 * @param ncid identifies the netCDF file
 * @param varid the variable ID to be read
 * @param ioid: the I/O description ID as passed back by
 * PIOc_InitDecomp().
 * @param arraylen: the length of the array to be read. This
 * is the length of the distrubited array. That is, the length of
 * the portion of the data that is on the processor.
 * @param array: pointer to the data to be read. This is a
 * pointer to the distributed portion of the array that is on this
 * processor.
 * @return 0 for success, error code otherwise.
 * @ingroup PIO_read_darray
 * @author Jim Edwards, Ed Hartnett
 */
int PIOc_read_darray(int ncid, int varid, int ioid, PIO_Offset arraylen,
                     void *array)
{
    iosystem_desc_t *ios;  /* Pointer to io system information. */
    file_desc_t *file;     /* Pointer to file information. */
    io_desc_t *iodesc;     /* Pointer to IO description information. */
    void *iobuf = NULL;    /* holds the data as read on the io node. */
    size_t rlen = 0;       /* the length of data in iobuf. */
    int ierr;           /* Return code. */

#ifdef TIMING
    GPTLstart("PIO:PIOc_read_darray");
#endif
    /* Get the file info. */
    if ((ierr = pio_get_file(ncid, &file)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);
    ios = file->iosystem;

    LOG((1, "PIOc_read_darray (ncid=%d (%s), varid=%d (%s)", ncid, file->fname, varid, file->varlist[varid].vname));

    /* Get the iodesc. */
    if (!(iodesc = pio_get_iodesc_from_id(ioid)))
        return pio_err(ios, file, PIO_EBADID, __FILE__, __LINE__);
    pioassert(iodesc->rearranger == PIO_REARR_BOX || iodesc->rearranger == PIO_REARR_SUBSET,
              "unknown rearranger", __FILE__, __LINE__);

#ifdef PIO_MICRO_TIMING
    mtimer_start(file->varlist[varid].rd_mtimer);
#endif

    /* ??? */
    if (ios->iomaster == MPI_ROOT)
        rlen = iodesc->maxiobuflen;
    else
        rlen = iodesc->llen;

    if(!ios->async || !ios->ioproc)
    {
        if(file->varlist[varid].vrsize == 0)
        {
            ierr = calc_var_rec_sz(ncid, varid);
            if(ierr != PIO_NOERR)
            {
                LOG((1, "Unable to calculate the variable record size"));
            }
        }
    }

    file->varlist[varid].rb_pend += file->varlist[varid].vrsize;
    file->rb_pend += file->varlist[varid].vrsize;

    /* Allocate a buffer for one record. */
    if (ios->ioproc && rlen > 0)
        if (!(iobuf = bget(iodesc->mpitype_size * rlen)))
            return pio_err(ios, file, PIO_ENOMEM, __FILE__, __LINE__);

    /* Call the correct darray read function based on iotype. */
    switch (file->iotype)
    {
    case PIO_IOTYPE_NETCDF:
    case PIO_IOTYPE_NETCDF4C:
        if ((ierr = pio_read_darray_nc_serial(file, iodesc, varid, iobuf)))
                return pio_err(ios, file, ierr, __FILE__, __LINE__);
        break;
    case PIO_IOTYPE_PNETCDF:
    case PIO_IOTYPE_NETCDF4P:
        if ((ierr = pio_read_darray_nc(file, iodesc, varid, iobuf)))
            return pio_err(ios, file, ierr, __FILE__, __LINE__);
        break;
    default:
        return pio_err(NULL, NULL, PIO_EBADIOTYPE, __FILE__, __LINE__);
    }

#ifdef PIO_MICRO_TIMING
    mtimer_start(file->varlist[varid].rd_rearr_mtimer);
#endif
    /* Rearrange the data. */
    if ((ierr = rearrange_io2comp(ios, iodesc, iobuf, array)))
        return pio_err(ios, file, ierr, __FILE__, __LINE__);

#ifdef PIO_MICRO_TIMING
    mtimer_stop(file->varlist[varid].rd_rearr_mtimer, get_var_desc_str(ncid, varid, NULL));
#endif
    /* We don't use non-blocking reads */
    file->varlist[varid].rb_pend = 0;
    file->rb_pend = 0;

    /* Free the buffer. */
    if (rlen > 0)
        brel(iobuf);

#ifdef PIO_MICRO_TIMING
    mtimer_stop(file->varlist[varid].rd_mtimer, get_var_desc_str(ncid, varid, NULL));
#endif
#ifdef TIMING
    GPTLstop("PIO:PIOc_read_darray");
#endif
    return PIO_NOERR;
}
