/*
 * 7Zip support routines for PhysicsFS.
 *
 * Please see the file lzma.txt in the lzma/ directory.
 *
 *  This file was written by Dennis Schridde, with some peeking at "7zMain.c"
 *   by Igor Pavlov.
 */

#define __PHYSICSFS_INTERNAL__
#include "physfs_internal.h"

#if PHYSFS_SUPPORTS_7Z

#include "lzma/C/7zCrc.h"
#include "lzma/C/Archive/7z/7zIn.h"
#include "lzma/C/Archive/7z/7zExtract.h"


/* 7z internal from 7zIn.c */
extern int TestSignatureCandidate(Byte *testBytes);


#ifdef _LZMA_IN_CB
# define BUFFER_SIZE (1 << 12)
#endif /* _LZMA_IN_CB */


/*
 * Carries filestream metadata through 7z
 */
typedef struct _SZfileinstream
{
    ISzAlloc allocImp; /* Allocation implementation, used by 7z */
    ISzAlloc allocTempImp; /* Temporary allocation implementation, used by 7z */
    ISzInStream inStream; /* Input stream with read callbacks, used by 7z */
    PHYSFS_Io *io;  /* Filehandle, used by read implementation */
#ifdef _LZMA_IN_CB
    Byte buffer[BUFFER_SIZE]; /* Buffer, used by read implementation */
#endif /* _LZMA_IN_CB */
} SZfileinstream;

/*
 * In the 7z format archives are splited into blocks, those are called folders
 * Set by SZ_read()
*/
typedef struct _SZfolder
{
    PHYSFS_uint32 index; /* Index of folder in archive */
    PHYSFS_uint32 references; /* Number of files using this block */
    PHYSFS_uint8 *cache; /* Cached folder */
    size_t size; /* Size of folder */
} SZfolder;

/*
 * Set by SZ_openArchive(), except folder which gets it's values
 *  in SZ_read()
 */
typedef struct _SZarchive
{
    struct _SZfile *files; /* Array of files, size == archive->db.Database.NumFiles */
    SZfolder *folders; /* Array of folders, size == archive->db.Database.NumFolders */
    CArchiveDatabaseEx db; /* For 7z: Database */
    SZfileinstream stream; /* For 7z: Input file incl. read and seek callbacks */
} SZarchive;

/* Set by SZ_openArchive(), except offset which is set by SZ_read() */
typedef struct _SZfile
{
    PHYSFS_uint32 index; /* Index of file in archive */
    SZarchive *archive; /* Link to corresponding archive */
    SZfolder *folder; /* Link to corresponding folder */
    CFileItem *item; /* For 7z: File info, eg. name, size */
    size_t offset; /* Offset in folder */
    size_t position; /* Current "virtual" position in file */
} SZfile;


/* Memory management implementations to be passed to 7z */

static void *sz_alloc(size_t size)
{
    return ((size == 0) ? NULL : allocator.Malloc(size));
} /* sz_alloc */


static void sz_free(void *address)
{
    if (address != NULL)
        allocator.Free(address);
} /* sz_free */


/* Filesystem implementations to be passed to 7z */

#ifdef _LZMA_IN_CB

/*
 * Read implementation, to be passed to 7z
 * WARNING: If the ISzInStream in 'object' is not contained in a valid SZfileinstream this _will_ break horribly!
 */
static SZ_RESULT sz_file_read(void *object, void **buffer, size_t maxReqSize,
                        size_t *processedSize)
{
    SZfileinstream *s = (SZfileinstream *)(object - offsetof(SZfileinstream, inStream)); /* HACK! */
    PHYSFS_sint64 processedSizeLoc = 0;

    if (maxReqSize > BUFFER_SIZE)
        maxReqSize = BUFFER_SIZE;
    processedSizeLoc = s->io->read(s->io, s->buffer, maxReqSize);
    *buffer = s->buffer;
    if (processedSize != NULL)
        *processedSize = (size_t) processedSizeLoc;

    return SZ_OK;
} /* sz_file_read */

#else

/*
 * Read implementation, to be passed to 7z
 * WARNING: If the ISzInStream in 'object' is not contained in a valid SZfileinstream this _will_ break horribly!
 */
static SZ_RESULT sz_file_read(void *object, void *buffer, size_t size,
                        size_t *processedSize)
{
    SZfileinstream *s = (SZfileinstream *)((unsigned long)object - offsetof(SZfileinstream, inStream)); /* HACK! */
    const size_t processedSizeLoc = s->io->read(s->io, buffer, size);
    if (processedSize != NULL)
        *processedSize = processedSizeLoc;
    return SZ_OK;
} /* sz_file_read */

#endif

/*
 * Seek implementation, to be passed to 7z
 * WARNING: If the ISzInStream in 'object' is not contained in a valid SZfileinstream this _will_ break horribly!
 */
static SZ_RESULT sz_file_seek(void *object, CFileSize pos)
{
    SZfileinstream *s = (SZfileinstream *)((unsigned long)object - offsetof(SZfileinstream, inStream)); /* HACK! */
    if (s->io->seek(s->io, (PHYSFS_uint64) pos))
        return SZ_OK;
    return SZE_FAIL;
} /* sz_file_seek */


/*
 * Translate Microsoft FILETIME (used by 7zip) into UNIX timestamp
 */
static PHYSFS_sint64 sz_filetime_to_unix_timestamp(CArchiveFileTime *ft)
{
    /* MS counts in nanoseconds ... */
    const PHYSFS_uint64 FILETIME_NANOTICKS_PER_SECOND = __PHYSFS_UI64(10000000);
    /* MS likes to count seconds since 01.01.1601 ... */
    const PHYSFS_uint64 FILETIME_UNIX_DIFF = __PHYSFS_UI64(11644473600);

    PHYSFS_uint64 filetime = ft->Low | ((PHYSFS_uint64)ft->High << 32);
    return filetime/FILETIME_NANOTICKS_PER_SECOND - FILETIME_UNIX_DIFF;
} /* sz_filetime_to_unix_timestamp */


/*
 * Compare a file with a given name, C89 stdlib variant
 * Used for sorting
 */
static int sz_file_cmp_stdlib(const void *key, const void *object)
{
    const char *name = (const char *) key;
    SZfile *file = (SZfile *) object;
    return strcmp(name, file->item->Name);
} /* sz_file_cmp_stdlib */


/*
 * Compare two files with each other based on the name
 * Used for sorting
 */
static int sz_file_cmp(void *_a, size_t one, size_t two)
{
    SZfile *files = (SZfile *) _a;
    return strcmp(files[one].item->Name, files[two].item->Name);
} /* sz_file_cmp */


/*
 * Swap two entries in the file array
 */
static void sz_file_swap(void *_a, size_t one, size_t two)
{
    SZfile tmp;
    SZfile *first = &(((SZfile *) _a)[one]);
    SZfile *second = &(((SZfile *) _a)[two]);
    memcpy(&tmp, first, sizeof (SZfile));
    memcpy(first, second, sizeof (SZfile));
    memcpy(second, &tmp, sizeof (SZfile));
} /* sz_file_swap */


/*
 * Find entry 'name' in 'archive'
 */
static SZfile * sz_find_file(const SZarchive *archive, const char *name)
{
    SZfile *file = bsearch(name, archive->files, archive->db.Database.NumFiles, sizeof(*archive->files), sz_file_cmp_stdlib); /* FIXME: Should become __PHYSFS_search!!! */

    BAIL_IF_MACRO(file == NULL, PHYSFS_ERR_NOT_FOUND, NULL);

    return file;
} /* sz_find_file */


/*
 * Load metadata for the file at given index
 */
static int sz_file_init(SZarchive *archive, PHYSFS_uint32 fileIndex)
{
    SZfile *file = &archive->files[fileIndex];
    PHYSFS_uint32 folderIndex = archive->db.FileIndexToFolderIndexMap[fileIndex];

    file->index = fileIndex; /* Store index into 7z array, since we sort our own. */
    file->archive = archive;
    file->folder = (folderIndex != (PHYSFS_uint32)-1 ? &archive->folders[folderIndex] : NULL); /* Directories don't have a folder (they contain no own data...) */
    file->item = &archive->db.Database.Files[fileIndex]; /* Holds crucial data and is often referenced -> Store link */
    file->position = 0;
    file->offset = 0; /* Offset will be set by SZ_read() */

    return 1;
} /* sz_file_init */


/*
 * Load metadata for all files
 */
static int sz_files_init(SZarchive *archive)
{
    PHYSFS_uint32 fileIndex = 0, numFiles = archive->db.Database.NumFiles;

    for (fileIndex = 0; fileIndex < numFiles; fileIndex++ )
    {
        if (!sz_file_init(archive, fileIndex))
        {
            return 0; /* FALSE on failure */
        }
    } /* for */

   __PHYSFS_sort(archive->files, (size_t) numFiles, sz_file_cmp, sz_file_swap);

    return 1;
} /* sz_files_init */


/*
 * Initialise specified archive
 */
static void sz_archive_init(SZarchive *archive)
{
    memset(archive, 0, sizeof(*archive));

    /* Prepare callbacks for 7z */
    archive->stream.inStream.Read = sz_file_read;
    archive->stream.inStream.Seek = sz_file_seek;

    archive->stream.allocImp.Alloc = sz_alloc;
    archive->stream.allocImp.Free = sz_free;

    archive->stream.allocTempImp.Alloc = sz_alloc;
    archive->stream.allocTempImp.Free = sz_free;
} /* sz_archive_init */


/*
 * Deinitialise archive
 */
static void sz_archive_exit(SZarchive *archive)
{
    /* Free arrays */
    allocator.Free(archive->folders);
    allocator.Free(archive->files);
    allocator.Free(archive);
} /* sz_archive_exit */

/*
 * Wrap all 7z calls in this, so the physfs error state is set appropriately.
 */
static int sz_err(SZ_RESULT rc)
{
    switch (rc)
    {
        case SZ_OK: /* Same as LZMA_RESULT_OK */
            break;
        case SZE_DATA_ERROR: /* Same as LZMA_RESULT_DATA_ERROR */
            PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT); /*!!!FIXME: was "PHYSFS_ERR_DATA_ERROR" */
            break;
        case SZE_OUTOFMEMORY:
            PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
            break;
        case SZE_CRC_ERROR:
            PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
            break;
        case SZE_NOTIMPL:
            PHYSFS_setErrorCode(PHYSFS_ERR_UNSUPPORTED);
            break;
        case SZE_FAIL:
            PHYSFS_setErrorCode(PHYSFS_ERR_OTHER_ERROR);  /* !!! FIXME: right? */
            break;
        case SZE_ARCHIVE_ERROR:
            PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);  /* !!! FIXME: right? */
            break;
        default:
            PHYSFS_setErrorCode(PHYSFS_ERR_OTHER_ERROR);
    } /* switch */

    return rc;
} /* sz_err */


static PHYSFS_sint64 SZ_read(PHYSFS_Io *io, void *outBuf, PHYSFS_uint64 len)
{
    SZfile *file = (SZfile *) io->opaque;

    size_t wantedSize = (size_t) len;
    const size_t remainingSize = file->item->Size - file->position;
    size_t fileSize = 0;

    BAIL_IF_MACRO(wantedSize == 0, ERRPASS, 0); /* quick rejection. */
    BAIL_IF_MACRO(remainingSize == 0, PHYSFS_ERR_PAST_EOF, 0);

    if (wantedSize > remainingSize)
        wantedSize = remainingSize;

    /* Only decompress the folder if it is not already cached */
    if (file->folder->cache == NULL)
    {
        const int rc = sz_err(SzExtract(
            &file->archive->stream.inStream, /* compressed data */
            &file->archive->db, /* 7z's database, containing everything */
            file->index, /* Index into database arrays */
            /* Index of cached folder, will be changed by SzExtract */
            &file->folder->index,
            /* Cache for decompressed folder, allocated/freed by SzExtract */
            &file->folder->cache,
            /* Size of cache, will be changed by SzExtract */
            &file->folder->size,
            /* Offset of this file inside the cache, set by SzExtract */
            &file->offset,
            &fileSize, /* Size of this file */
            &file->archive->stream.allocImp,
            &file->archive->stream.allocTempImp));

        if (rc != SZ_OK)
            return -1;
    } /* if */

    /* Copy wanted bytes over from cache to outBuf */
    memcpy(outBuf, (file->folder->cache + file->offset + file->position),
            wantedSize);
    file->position += wantedSize; /* Increase virtual position */

    return wantedSize;
} /* SZ_read */


static PHYSFS_sint64 SZ_write(PHYSFS_Io *io, const void *b, PHYSFS_uint64 len)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, -1);
} /* SZ_write */


static PHYSFS_sint64 SZ_tell(PHYSFS_Io *io)
{
    SZfile *file = (SZfile *) io->opaque;
    return file->position;
} /* SZ_tell */


static int SZ_seek(PHYSFS_Io *io, PHYSFS_uint64 offset)
{
    SZfile *file = (SZfile *) io->opaque;

    BAIL_IF_MACRO(offset > file->item->Size, PHYSFS_ERR_PAST_EOF, 0);

    file->position = offset; /* We only use a virtual position... */

    return 1;
} /* SZ_seek */


static PHYSFS_sint64 SZ_length(PHYSFS_Io *io)
{
    const SZfile *file = (SZfile *) io->opaque;
    return (file->item->Size);
} /* SZ_length */


static PHYSFS_Io *SZ_duplicate(PHYSFS_Io *_io)
{
    /* !!! FIXME: this archiver needs to be reworked to allow multiple
     * !!! FIXME:  opens before we worry about duplication. */
    BAIL_MACRO(PHYSFS_ERR_UNSUPPORTED, NULL);
} /* SZ_duplicate */


static int SZ_flush(PHYSFS_Io *io) { return 1;  /* no write support. */ }


static void SZ_destroy(PHYSFS_Io *io)
{
    SZfile *file = (SZfile *) io->opaque;

    if (file->folder != NULL)
    {
        /* Only decrease refcount if someone actually requested this file... Prevents from overflows and close-on-open... */
        if (file->folder->references > 0)
            file->folder->references--;
        if (file->folder->references == 0)
        {
            /* Free the cache which might have been allocated by SZ_read() */
            allocator.Free(file->folder->cache);
            file->folder->cache = NULL;
        }
        /* !!! FIXME: we don't free (file) or (file->folder)?! */
    } /* if */
} /* SZ_destroy */


static const PHYSFS_Io SZ_Io =
{
    CURRENT_PHYSFS_IO_API_VERSION, NULL,
    SZ_read,
    SZ_write,
    SZ_seek,
    SZ_tell,
    SZ_length,
    SZ_duplicate,
    SZ_flush,
    SZ_destroy
};


static void *SZ_openArchive(PHYSFS_Io *io, const char *name, int forWriting)
{
    PHYSFS_uint8 sig[k7zSignatureSize];
    size_t len = 0;
    SZarchive *archive = NULL;

    assert(io != NULL);  /* shouldn't ever happen. */

    BAIL_IF_MACRO(forWriting, PHYSFS_ERR_READ_ONLY, NULL);

    if (io->read(io, sig, k7zSignatureSize) != k7zSignatureSize)
        return 0;
    BAIL_IF_MACRO(!TestSignatureCandidate(sig), PHYSFS_ERR_UNSUPPORTED, NULL);
    BAIL_IF_MACRO(!io->seek(io, 0), ERRPASS, NULL);

    archive = (SZarchive *) allocator.Malloc(sizeof (SZarchive));
    BAIL_IF_MACRO(archive == NULL, PHYSFS_ERR_OUT_OF_MEMORY, NULL);

    sz_archive_init(archive);
    archive->stream.io = io;

    CrcGenerateTable();
    SzArDbExInit(&archive->db);
    if (sz_err(SzArchiveOpen(&archive->stream.inStream,
                             &archive->db,
                             &archive->stream.allocImp,
                             &archive->stream.allocTempImp)) != SZ_OK)
    {
        SzArDbExFree(&archive->db, sz_free);
        sz_archive_exit(archive);
        return NULL; /* Error is set by sz_err! */
    } /* if */

    len = archive->db.Database.NumFiles * sizeof (SZfile);
    archive->files = (SZfile *) allocator.Malloc(len);
    if (archive->files == NULL)
    {
        SzArDbExFree(&archive->db, sz_free);
        sz_archive_exit(archive);
        BAIL_MACRO(PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    }

    /*
     * Init with 0 so we know when a folder is already cached
     * Values will be set by SZ_openRead()
     */
    memset(archive->files, 0, len);

    len = archive->db.Database.NumFolders * sizeof (SZfolder);
    archive->folders = (SZfolder *) allocator.Malloc(len);
    if (archive->folders == NULL)
    {
        SzArDbExFree(&archive->db, sz_free);
        sz_archive_exit(archive);
        BAIL_MACRO(PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    }

    /*
     * Init with 0 so we know when a folder is already cached
     * Values will be set by SZ_read()
     */
    memset(archive->folders, 0, len);

    if(!sz_files_init(archive))
    {
        SzArDbExFree(&archive->db, sz_free);
        sz_archive_exit(archive);
        BAIL_MACRO(PHYSFS_ERR_OTHER_ERROR, NULL);
    }

    return archive;
} /* SZ_openArchive */


/*
 * Moved to seperate function so we can use alloca then immediately throw
 *  away the allocated stack space...
 */
static void doEnumCallback(PHYSFS_EnumFilesCallback cb, void *callbackdata,
                           const char *odir, const char *str, size_t flen)
{
    char *newstr = __PHYSFS_smallAlloc(flen + 1);
    if (newstr == NULL)
        return;

    memcpy(newstr, str, flen);
    newstr[flen] = '\0';
    cb(callbackdata, odir, newstr);
    __PHYSFS_smallFree(newstr);
} /* doEnumCallback */


static void SZ_enumerateFiles(void *opaque, const char *dname,
                                PHYSFS_EnumFilesCallback cb,
                                const char *origdir, void *callbackdata)
{
    size_t dlen = strlen(dname),
           dlen_inc = dlen + ((dlen > 0) ? 1 : 0);
    SZarchive *archive = (SZarchive *) opaque;
    SZfile *file = NULL,
            *lastFile = &archive->files[archive->db.Database.NumFiles];
        if (dlen)
        {
            file = sz_find_file(archive, dname);
            if (file != NULL) /* if 'file' is NULL it should stay so, otherwise errors will not be handled */
                file += 1;
        }
        else
        {
            file = archive->files;
        }

    BAIL_IF_MACRO(file == NULL, PHYSFS_ERR_NOT_FOUND, );

    while (file < lastFile)
    {
        const char * fname = file->item->Name;
        const char * dirNameEnd = fname + dlen_inc;

        if (strncmp(dname, fname, dlen) != 0) /* Stop after mismatch, archive->files is sorted */
            break;

        if (strchr(dirNameEnd, '/')) /* Skip subdirs */
        {
            file++;
            continue;
        }

        /* Do the actual callback... */
        doEnumCallback(cb, callbackdata, origdir, dirNameEnd, strlen(dirNameEnd));

        file++;
    }
} /* SZ_enumerateFiles */


static PHYSFS_Io *SZ_openRead(void *opaque, const char *name)
{
    SZarchive *archive = (SZarchive *) opaque;
    SZfile *file = sz_find_file(archive, name);
    PHYSFS_Io *io = NULL;

    BAIL_IF_MACRO(file == NULL, PHYSFS_ERR_NOT_FOUND, NULL);
    BAIL_IF_MACRO(file->folder == NULL, PHYSFS_ERR_NOT_A_FILE, NULL);

    file->position = 0;
    file->folder->references++; /* Increase refcount for automatic cleanup... */

    io = (PHYSFS_Io *) allocator.Malloc(sizeof (PHYSFS_Io));
    BAIL_IF_MACRO(io == NULL, PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    memcpy(io, &SZ_Io, sizeof (*io));
    io->opaque = file;

    return io;
} /* SZ_openRead */


static PHYSFS_Io *SZ_openWrite(void *opaque, const char *filename)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, NULL);
} /* SZ_openWrite */


static PHYSFS_Io *SZ_openAppend(void *opaque, const char *filename)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, NULL);
} /* SZ_openAppend */


static void SZ_closeArchive(void *opaque)
{
    SZarchive *archive = (SZarchive *) opaque;

#if 0  /* !!! FIXME: you shouldn't have to do this. */
    PHYSFS_uint32 fileIndex = 0, numFiles = archive->db.Database.NumFiles;
    for (fileIndex = 0; fileIndex < numFiles; fileIndex++)
    {
        SZ_fileClose(&archive->files[fileIndex]);
    } /* for */
#endif

    SzArDbExFree(&archive->db, sz_free);
    archive->stream.io->destroy(archive->stream.io);
    sz_archive_exit(archive);
} /* SZ_closeArchive */


static int SZ_remove(void *opaque, const char *name)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, 0);
} /* SZ_remove */


static int SZ_mkdir(void *opaque, const char *name)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, 0);
} /* SZ_mkdir */

static int SZ_stat(void *opaque, const char *filename, PHYSFS_Stat *stat)
{
    const SZarchive *archive = (const SZarchive *) opaque;
    const SZfile *file = sz_find_file(archive, filename);

    if (!file)
        return 0;

    if(file->item->IsDirectory)
    {
        stat->filesize = 0;
        stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
    } /* if */
    else
    {
        stat->filesize = (PHYSFS_sint64) file->item->Size;
        stat->filetype = PHYSFS_FILETYPE_REGULAR;
    } /* else */

    /* !!! FIXME: the 0's should be -1's? */
    if (file->item->IsLastWriteTimeDefined)
        stat->modtime = sz_filetime_to_unix_timestamp(&file->item->LastWriteTime);
    else
        stat->modtime = 0;

    /* real create and accesstype are currently not in the lzma SDK */
    stat->createtime = stat->modtime;
    stat->accesstime = 0;

    stat->readonly = 1;  /* 7zips are always read only */

    return 1;
} /* SZ_stat */


const PHYSFS_Archiver __PHYSFS_Archiver_SZ =
{
    CURRENT_PHYSFS_ARCHIVER_API_VERSION,
    {
        "7Z",
        "7Zip format",
        "Dennis Schridde <devurandom@gmx.net>",
        "http://icculus.org/physfs/",
        0,  /* supportsSymlinks */
    },
    SZ_openArchive,
    SZ_enumerateFiles,
    SZ_openRead,
    SZ_openWrite,
    SZ_openAppend,
    SZ_remove,
    SZ_mkdir,
    SZ_stat,
    SZ_closeArchive
};

#endif  /* defined PHYSFS_SUPPORTS_7Z */

/* end of archiver_7z.c ... */

