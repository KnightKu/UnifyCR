/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2017, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyCR.
 * For details, see https://github.com/LLNL/UnifyCR.
 * Please read https://github.com/LLNL/UnifyCR/LICENSE for full license text.
 */

/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 *
 * Written by: Teng Wang, Adam Moody, Weikuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICENSE for full license text.
 */

/*
 * Copyright (c) 2013, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * code Written by
 *   Raghunath Rajachandrasekar <rajachan@cse.ohio-state.edu>
 *   Kathryn Mohror <kathryn@llnl.gov>
 *   Adam Moody <moody20@llnl.gov>
 * All rights reserved.
 * This file is part of CRUISE.
 * For details, see https://github.com/hpc/cruise
 * Please also read this file LICENSE.CRUISE
 */

#include "unifycr-fixed.h"
#include "unifycr_log.h"

static inline
unifycr_chunkmeta_t* filemeta_get_chunkmeta(const unifycr_filemeta_t* meta,
                                            int cid)
{
    unifycr_chunkmeta_t* chunkmeta = NULL;
    uint64_t limit = 0;

    if (unifycr_use_memfs) {
        limit += unifycr_max_chunks;
    }

    if (unifycr_use_spillover) {
        limit += unifycr_spillover_max_chunks;
    }

    if (meta && (cid >= 0 && cid < limit)) {
        chunkmeta = &unifycr_chunkmetas[meta->chunkmeta_idx + cid];
    }

    return chunkmeta;
}

/* given a file id and logical chunk id, return pointer to meta data
 * for specified chunk, return NULL if not found */
static inline unifycr_chunkmeta_t* unifycr_get_chunkmeta(int fid, int cid)
{
    /* lookup file meta data for specified file id */
    unifycr_filemeta_t* meta = unifycr_get_meta_from_fid(fid);

    return filemeta_get_chunkmeta(meta, cid);
}

/* ---------------------------------------
 * Operations on file chunks
 * --------------------------------------- */

/* given a logical chunk id and an offset within that chunk, return the pointer
 * to the memory location corresponding to that location */
static inline void* unifycr_compute_chunk_buf(const unifycr_filemeta_t* meta,
                                              int cid, off_t offset)
{
    /* get pointer to chunk meta */
    const unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, cid);

    /* identify physical chunk id */
    int physical_id = chunk_meta->id;

    /* compute the start of the chunk */
    char* start = NULL;
    if (physical_id < unifycr_max_chunks) {
        start = unifycr_chunks + ((long)physical_id << unifycr_chunk_bits);
    } else {
        /* chunk is in spill over */
        LOGERR("wrong chunk ID");
        return NULL;
    }

    /* now add offset */
    char* buf = start + offset;
    return (void*)buf;
}

/* given a chunk id and an offset within that chunk, return the offset
 * in the spillover file corresponding to that location */
static inline off_t unifycr_compute_spill_offset(const unifycr_filemeta_t* meta,
                                                 int cid, off_t offset)
{
    /* get pointer to chunk meta */
    const unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, cid);

    /* identify physical chunk id */
    int physical_id = chunk_meta->id;

    /* compute start of chunk in spill over device */
    off_t start = 0;
    if (physical_id < unifycr_max_chunks) {
        LOGERR("wrong spill-chunk ID");
        return -1;
    } else {
        /* compute buffer loc within spillover device chunk */
        /* account for the unifycr_max_chunks added to identify location when
         * grabbing this chunk */
        start = ((long)(physical_id - unifycr_max_chunks) << unifycr_chunk_bits);
    }

    off_t buf = start + offset;
    return buf;
}

/* allocate a new chunk for the specified file and logical chunk id */
static int unifycr_chunk_alloc(int fid, unifycr_filemeta_t* meta, int chunk_id)
{
    /* get pointer to chunk meta data */
    unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, chunk_id);

    /* allocate a chunk and record its location */
    if (unifycr_use_memfs) {
        /* allocate a new chunk from memory */
        unifycr_stack_lock();
        int id = unifycr_stack_pop(free_chunk_stack);
        unifycr_stack_unlock();

        /* if we got one return, otherwise try spill over */
        if (id >= 0) {
            /* got a chunk from memory */
            chunk_meta->location = CHUNK_LOCATION_MEMFS;
            chunk_meta->id = id;
        } else if (unifycr_use_spillover) {
            /* shm segment out of space, grab a block from spill-over device */
            LOGDBG("getting blocks from spill-over device");

            /* TODO: missing lock calls? */
            /* add unifycr_max_chunks to identify chunk location */
            unifycr_stack_lock();
            id = unifycr_stack_pop(free_spillchunk_stack) + unifycr_max_chunks;
            unifycr_stack_unlock();
            if (id < unifycr_max_chunks) {
                LOGERR("spill-over device out of space (%d)", id);
                return UNIFYCR_ERROR_NOSPC;
            }

            /* got one from spill over */
            chunk_meta->location = CHUNK_LOCATION_SPILLOVER;
            chunk_meta->id = id;
        } else {
            /* spill over isn't available, so we're out of space */
            LOGERR("memfs out of space (%d)", id);
            return UNIFYCR_ERROR_NOSPC;
        }
    } else if (unifycr_use_spillover) {
        /* memory file system is not enabled, but spill over is */

        /* shm segment out of space, grab a block from spill-over device */
        LOGDBG("getting blocks from spill-over device");

        /* TODO: missing lock calls? */
        /* add unifycr_max_chunks to identify chunk location */
        unifycr_stack_lock();
        int id = unifycr_stack_pop(free_spillchunk_stack) + unifycr_max_chunks;
        unifycr_stack_unlock();
        if (id < unifycr_max_chunks) {
            LOGERR("spill-over device out of space (%d)", id);
            return UNIFYCR_ERROR_NOSPC;
        }

        /* got one from spill over */
        chunk_meta->location = CHUNK_LOCATION_SPILLOVER;
        chunk_meta->id = id;
    } else {
        /* don't know how to allocate chunk */
        chunk_meta->location = CHUNK_LOCATION_NULL;
        return UNIFYCR_ERROR_IO;
    }

    return UNIFYCR_SUCCESS;
}

static int unifycr_chunk_free(int fid, unifycr_filemeta_t* meta, int chunk_id)
{
    /* get pointer to chunk meta data */
    unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, chunk_id);

    /* get physical id of chunk */
    int id = chunk_meta->id;
    LOGDBG("free chunk %d from location %d", id, chunk_meta->location);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        unifycr_stack_lock();
        unifycr_stack_push(free_chunk_stack, id);
        unifycr_stack_unlock();
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* TODO: free spill over chunk */
    } else {
        /* unkwown chunk location */
        LOGERR("unknown chunk location %d", chunk_meta->location);
        return UNIFYCR_ERROR_IO;
    }

    /* update location of chunk */
    chunk_meta->location = CHUNK_LOCATION_NULL;

    return UNIFYCR_SUCCESS;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int unifycr_chunk_read(
    unifycr_filemeta_t* meta, /* pointer to file meta data */
    int chunk_id,            /* logical chunk id to read data from */
    off_t chunk_offset,      /* logical offset within chunk to read from */
    void* buf,               /* buffer to store data to */
    size_t count)            /* number of bytes to read */
{
    /* get chunk meta data */
    unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, chunk_id);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to read data */
        void* chunk_buf = unifycr_compute_chunk_buf(
            meta, chunk_id, chunk_offset);
        memcpy(buf, chunk_buf, count);
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so read from file descriptor */
        //MAP_OR_FAIL(pread);
        off_t spill_offset = unifycr_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = pread(unifycr_spilloverblock, buf, count, spill_offset);
        if (rc < 0) {
            return unifycr_errno_map_to_err(rc);
        }
    } else {
        /* unknown chunk type */
        LOGERR("unknown chunk type");
        return UNIFYCR_ERROR_IO;
    }

    /* assume read was successful if we get to here */
    return UNIFYCR_SUCCESS;
}

/*
 * given an index, split it into multiple indices whose range is equal or smaller
 * than slice_range size
 * @param cur_idx: the index to split
 * @param slice_range: the slice size of the key-value store
 * @return index_set: the set of split indices
 * */
int unifycr_split_index(unifycr_index_t* cur_idx, index_set_t* index_set,
                        long slice_range)
{

    long cur_idx_start = cur_idx->file_pos;
    long cur_idx_end = cur_idx->file_pos + cur_idx->length - 1;

    long cur_slice_start = cur_idx->file_pos / slice_range * slice_range;
    long cur_slice_end = cur_slice_start + slice_range - 1;


    index_set->count = 0;

    long cur_mem_pos = cur_idx->mem_pos;
    if (cur_idx_end <= cur_slice_end) {
        /*
        cur_slice_start                                  cur_slice_end
                         cur_idx_start      cur_idx_end

        */
        index_set->idxes[index_set->count] = *cur_idx;
        index_set->count++;

    } else {
        /*
        cur_slice_start                     cur_slice_endnext_slice_start                   next_slice_end
                         cur_idx_start                                      cur_idx_end

        */
        index_set->idxes[index_set->count] = *cur_idx;
        index_set->idxes[index_set->count].length =
            cur_slice_end - cur_idx_start + 1;

        cur_mem_pos += index_set->idxes[index_set->count].length;

        cur_slice_start = cur_slice_end + 1;
        cur_slice_end = cur_slice_start + slice_range - 1;
        index_set->count++;

        while (1) {
            if (cur_idx_end <= cur_slice_end) {
                break;
            }

            index_set->idxes[index_set->count].fid = cur_idx->fid;
            index_set->idxes[index_set->count].file_pos = cur_slice_start;
            index_set->idxes[index_set->count].length = slice_range;
            index_set->idxes[index_set->count].mem_pos = cur_mem_pos;
            cur_mem_pos += index_set->idxes[index_set->count].length;

            cur_slice_start = cur_slice_end + 1;
            cur_slice_end = cur_slice_start + slice_range - 1;
            index_set->count++;

        }

        index_set->idxes[index_set->count].fid = cur_idx->fid;
        index_set->idxes[index_set->count].file_pos = cur_slice_start;
        index_set->idxes[index_set->count].length = cur_idx_end - cur_slice_start + 1;
        index_set->idxes[index_set->count].mem_pos = cur_mem_pos;
        index_set->count++;
    }

    return 0;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int unifycr_logio_chunk_write(
    int fid,                  /* local file id */
    long pos,                 /* write offset inside the file */
    unifycr_filemeta_t* meta, /* pointer to file meta data */
    int chunk_id,             /* logical chunk id to write to */
    off_t chunk_offset,       /* logical offset within chunk to write to */
    const void* buf,          /* buffer holding data to be written */
    size_t count)             /* number of bytes to write */
{
    /* get chunk meta data */
    unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, chunk_id);

    if (chunk_meta->location != CHUNK_LOCATION_MEMFS &&
            chunk_meta->location != CHUNK_LOCATION_SPILLOVER) {
        /* unknown chunk type */
        LOGERR("unknown chunk type");
        return UNIFYCR_ERROR_IO;
    }

    /* determine location of chunk */
    off_t log_offset = 0;
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to write data */
        char* chunk_buf = unifycr_compute_chunk_buf(
            meta, chunk_id, chunk_offset);
        memcpy(chunk_buf, buf, count);

        log_offset = chunk_buf - unifycr_chunks;
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so write to file descriptor */
        //MAP_OR_FAIL(pwrite);
        off_t spill_offset = unifycr_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = __real_pwrite(unifycr_spilloverblock, buf, count, spill_offset);
        if (rc < 0)  {
            LOGERR("pwrite failed: errno=%d (%s)", errno, strerror(errno));
        }

        log_offset = spill_offset + unifycr_max_chunks * (1 << unifycr_chunk_bits);
    }

    /* find the corresponding file attr entry and update attr*/
    unifycr_file_attr_t tmp_meta_entry;
    tmp_meta_entry.fid = fid;
    unifycr_file_attr_t* ptr_meta_entry
        = (unifycr_file_attr_t*)bsearch(&tmp_meta_entry,
                                        unifycr_fattrs.meta_entry,
                                        *unifycr_fattrs.ptr_num_entries,
                                        sizeof(unifycr_file_attr_t),
                                        compare_fattr);
    if (ptr_meta_entry !=  NULL) {
        ptr_meta_entry->file_attr.st_size = pos + count;
    }

    /* define an new index entry for this write operation */
    unifycr_index_t cur_idx;
    cur_idx.fid      = ptr_meta_entry->gfid;
    cur_idx.file_pos = pos;
    cur_idx.mem_pos  = log_offset;
    cur_idx.length   = count;

    /* split the write requests larger than unifycr_key_slice_range into
     * the ones smaller than unifycr_key_slice_range
     * */
    index_set_t tmp_index_set;
    memset(&tmp_index_set, 0, sizeof(tmp_index_set));
    unifycr_split_index(&cur_idx, &tmp_index_set,
                        unifycr_key_slice_range);

    /* lookup number of existing index entries */
    off_t num_entries = *(unifycr_indices.ptr_num_entries);

    /* number of new entries we may add */
    off_t tmp_entries = (off_t) tmp_index_set.count;

    /* check whether there is room to add new entries */
    if (num_entries + tmp_entries < unifycr_max_index_entries) {
        /* get pointer to index array */
        unifycr_index_t* idxs = unifycr_indices.index_entry;

        /* coalesce contiguous indices */
        int i = 0;
        if (num_entries > 0) {
            /* pointer to last element in index array */
            unifycr_index_t* prev_idx = &idxs[num_entries - 1];

            /* pointer to first element in temp list */
            unifycr_index_t* next_idx = &tmp_index_set.idxes[0];

            /* offset of last byte for last index in list */
            off_t prev_offset = prev_idx->file_pos + prev_idx->length;

            /* check whether last index and temp index refer to
             * contiguous bytes in the same file */
            if (prev_idx->fid == next_idx->fid &&
                    prev_offset   == next_idx->file_pos) {
                /* got contiguous bytes in the same file,
                 * check if both index values fall in the same slice */
                off_t prev_slice = prev_idx->file_pos / unifycr_key_slice_range;
                off_t next_slice = next_idx->file_pos / unifycr_key_slice_range;
                if (prev_slice == next_slice) {
                    /* index values also are in same slice,
                     * so append first index in temp list to
                     * last index in list */
                    prev_idx->length  += next_idx->length;

                    /* advance to next index in temp list */
                    i++;
                }
            }
        }

        /* pointer to temp index list */
        unifycr_index_t* newidxs = tmp_index_set.idxes;

        /* copy remaining items in temp index list to index list */
        while (i < tmp_index_set.count) {
            /* copy index fields */
            idxs[num_entries].fid      = newidxs[i].fid;
            idxs[num_entries].file_pos = newidxs[i].file_pos;
            idxs[num_entries].mem_pos  = newidxs[i].mem_pos;
            idxs[num_entries].length   = newidxs[i].length;

            /* advance to next element in each list */
            num_entries++;
            i++;
        }

        /* update number of entries in index array */
        (*unifycr_indices.ptr_num_entries) = num_entries;
    } else {
        /* TODO: no room to write additional index metadata entries,
         * swap out existing metadata buffer to disk*/
        printf("exhausted metadata");
    }

    /* assume read was successful if we get to here */
    return UNIFYCR_SUCCESS;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int unifycr_chunk_write(
    unifycr_filemeta_t* meta, /* pointer to file meta data */
    int chunk_id,            /* logical chunk id to write to */
    off_t chunk_offset,      /* logical offset within chunk to write to */
    const void* buf,         /* buffer holding data to be written */
    size_t count)            /* number of bytes to write */
{
    /* get chunk meta data */
    unifycr_chunkmeta_t* chunk_meta = filemeta_get_chunkmeta(meta, chunk_id);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to write data */
        void* chunk_buf = unifycr_compute_chunk_buf(
            meta, chunk_id, chunk_offset);
        memcpy(chunk_buf, buf, count);
//        _intel_fast_memcpy(chunk_buf, buf, count);
//        unifycr_memcpy(chunk_buf, buf, count);
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so write to file descriptor */
        //MAP_OR_FAIL(pwrite);
        off_t spill_offset = unifycr_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = pwrite(unifycr_spilloverblock, buf, count, spill_offset);
        if (rc < 0)  {
            LOGERR("pwrite failed: errno=%d (%s)", errno, strerror(errno));
        }

        /* TODO: check return code for errors */
    } else {
        /* unknown chunk type */
        LOGERR("unknown chunk type");
        return UNIFYCR_ERROR_IO;
    }

    /* assume read was successful if we get to here */
    return UNIFYCR_SUCCESS;
}

/* ---------------------------------------
 * Operations on file storage
 * --------------------------------------- */

/* if length is greater than reserved space, reserve space up to length */
int unifycr_fid_store_fixed_extend(int fid, unifycr_filemeta_t* meta,
                                   off_t length)
{
    /* determine whether we need to allocate more chunks */
    off_t maxsize = meta->chunks << unifycr_chunk_bits;
    if (length > maxsize) {
        /* compute number of additional bytes we need */
        off_t additional = length - maxsize;
        while (additional > 0) {
            /* check that we don't overrun max number of chunks for file */
            if (meta->chunks == unifycr_max_chunks + unifycr_spillover_max_chunks) {
                return UNIFYCR_ERROR_NOSPC;
            }

            /* allocate a new chunk */
            int rc = unifycr_chunk_alloc(fid, meta, meta->chunks);
            if (rc != UNIFYCR_SUCCESS) {
                LOGERR("failed to allocate chunk");
                return UNIFYCR_ERROR_NOSPC;
            }

            /* increase chunk count and subtract bytes from the number we need */
            meta->chunks++;
            additional -= unifycr_chunk_size;
        }
    }

    return UNIFYCR_SUCCESS;
}

/* if length is shorter than reserved space, give back space down to length */
int unifycr_fid_store_fixed_shrink(int fid, unifycr_filemeta_t* meta,
                                   off_t length)
{
    /* determine the number of chunks to leave after truncating */
    off_t num_chunks = 0;
    if (length > 0) {
        num_chunks = (length >> unifycr_chunk_bits) + 1;
    }

    /* clear off any extra chunks */
    while (meta->chunks > num_chunks) {
        meta->chunks--;
        unifycr_chunk_free(fid, meta, meta->chunks);
    }

    return UNIFYCR_SUCCESS;
}

/* read data from file stored as fixed-size chunks */
int unifycr_fid_store_fixed_read(int fid, unifycr_filemeta_t* meta, off_t pos,
                                 void* buf, size_t count)
{
    int rc;

    /* get pointer to position within first chunk */
    int chunk_id = pos >> unifycr_chunk_bits;
    off_t chunk_offset = pos & unifycr_chunk_mask;

    /* determine how many bytes remain in the current chunk */
    size_t remaining = unifycr_chunk_size - chunk_offset;
    if (count <= remaining) {
        /* all bytes for this read fit within the current chunk */
        rc = unifycr_chunk_read(meta, chunk_id, chunk_offset, buf, count);
    } else {
        /* read what's left of current chunk */
        char* ptr = (char*) buf;
        rc = unifycr_chunk_read(meta, chunk_id,
            chunk_offset, (void*)ptr, remaining);
        ptr += remaining;

        /* read from the next chunk */
        size_t processed = remaining;
        while (processed < count && rc == UNIFYCR_SUCCESS) {
            /* get pointer to start of next chunk */
            chunk_id++;

            /* compute size to read from this chunk */
            size_t num = count - processed;
            if (num > unifycr_chunk_size) {
                num = unifycr_chunk_size;
            }

            /* read data */
            rc = unifycr_chunk_read(meta, chunk_id, 0, (void*)ptr, num);
            ptr += num;

            /* update number of bytes written */
            processed += num;
        }
    }

    return rc;
}

/* write data to file stored as fixed-size chunks */
int unifycr_fid_store_fixed_write(int fid, unifycr_filemeta_t* meta, off_t pos,
                                  const void* buf, size_t count)
{
    int rc;

    /* get pointer to position within first chunk */
    int chunk_id;
    off_t chunk_offset;

    if (meta->storage == FILE_STORAGE_FIXED_CHUNK) {
        chunk_id = pos >> unifycr_chunk_bits;
        chunk_offset = pos & unifycr_chunk_mask;
    } else if (meta->storage == FILE_STORAGE_LOGIO) {
        chunk_id = meta->size >> unifycr_chunk_bits;
        chunk_offset = meta->size & unifycr_chunk_mask;
    } else {
        return UNIFYCR_ERROR_IO;
    }

    /* determine how many bytes remain in the current chunk */
    size_t remaining = unifycr_chunk_size - chunk_offset;
    if (count <= remaining) {
        /* all bytes for this write fit within the current chunk */
        if (meta->storage == FILE_STORAGE_FIXED_CHUNK) {
            rc = unifycr_chunk_write(meta, chunk_id, chunk_offset, buf, count);
        } else if (meta->storage == FILE_STORAGE_LOGIO) {
            rc = unifycr_logio_chunk_write(fid, pos, meta, chunk_id, chunk_offset,
                                           buf, count);
        } else {
            return UNIFYCR_ERROR_IO;
        }
    } else {
        /* otherwise, fill up the remainder of the current chunk */
        char* ptr = (char*) buf;
        if (meta->storage == FILE_STORAGE_FIXED_CHUNK) {
            rc = unifycr_chunk_write(meta, chunk_id,
                chunk_offset, (void*)ptr, remaining);
        } else if (meta->storage == FILE_STORAGE_LOGIO) {
            rc = unifycr_logio_chunk_write(fid, pos, meta, chunk_id,
                chunk_offset, (void*)ptr, remaining);
        } else {
            return UNIFYCR_ERROR_IO;
        }

        ptr += remaining;
        pos += remaining;

        /* then write the rest of the bytes starting from beginning
         * of chunks */
        size_t processed = remaining;
        while (processed < count && rc == UNIFYCR_SUCCESS) {
            /* get pointer to start of next chunk */
            chunk_id++;

            /* compute size to write to this chunk */
            size_t num = count - processed;
            if (num > unifycr_chunk_size) {
                num = unifycr_chunk_size;
            }

            /* write data */
            if (meta->storage == FILE_STORAGE_FIXED_CHUNK) {
                rc = unifycr_chunk_write(meta, chunk_id, 0, (void*)ptr, num);
            } else if (meta->storage == FILE_STORAGE_LOGIO)
                rc = unifycr_logio_chunk_write(fid, pos, meta, chunk_id, 0,
                                               (void*)ptr, num);
            else {
                return UNIFYCR_ERROR_IO;
            }
            ptr += num;
            pos += num;

            /* update number of bytes processed */
            processed += num;
        }
    }

    return rc;
}
