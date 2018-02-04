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

#include "7z.h"
#include "7zFile.h"
#include "7zCrc.h"

#define kInputBufSize ((size_t)1 << 18)

/*
 * Carries filestream metadata through 7z
 */
typedef struct _SZfileinstream
{
    ISeekInStream s; /* Read callbacks, used by 7z */
    PHYSFS_Io *io;  /* Filehandle, used by read implementation */
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
    CSzArEx db; /* For 7z: Database */
    ISzAllocPtr allocImp; /* Allocation implementation, used by 7z */
    ISzAllocPtr allocTempImp; /* Temporary allocation implementation, used by 7z */
    CLookToRead2 lookStream; /* Input stream wrapper with read callbacks, used by 7z */
    SZfileinstream inStream; /* Input stream with read callbacks, used by 7z */
    struct _SZfile *files; /* Array of files, size == archive->db.Database.NumFiles */
    SZfolder *folders; /* Array of folders, size == archive->db.Database.NumFolders */
} SZarchive;

/*
 * Legacy CSzFileItem structure
*/
typedef struct
{
  CNtfsFileTime MTime;
  UInt64 Size;
  //UInt32 Crc;
  //UInt32 Attrib;
  //Byte HasStream;
  Byte IsDir;
  //Byte IsAnti;
  //Byte CrcDefined;
  Byte MTimeDefined;
  //Byte AttribDefined;
} CSzFileItem;

/* Set by SZ_openArchive(), except offset which is set by SZ_read() */
typedef struct _SZfile
{
    PHYSFS_uint32 index; /* Index of file in archive */
    SZarchive *archive; /* Link to corresponding archive */
    SZfolder *folder; /* Link to corresponding folder */
    CSzFileItem *item; /* For 7z: File info, eg. name, size */
    size_t offset; /* Offset in folder */
    size_t position; /* Current "virtual" position in file */
    const char *name; /* Name of file */
} SZfile;


/* Memory management implementations to be passed to 7z */

static void *sz_alloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return ((size == 0) ? NULL : allocator.Malloc(size));
} /* sz_alloc */


static void sz_free(ISzAllocPtr p, void *address)
{
    (void)p;
    if (address != NULL)
        allocator.Free(address);
} /* sz_free */

static const ISzAlloc g_Alloc = { sz_alloc, sz_free };

/* Filesystem implementations to be passed to 7z */

/*
 * Read implementation, to be passed to 7z
 */
static SRes sz_file_read(const ISeekInStream *object, void *buffer, size_t *size)
{
    BAIL_IF(size == NULL, PHYSFS_ERR_INVALID_ARGUMENT, SZ_ERROR_PARAM);

    SZfileinstream *s = (SZfileinstream *)object; /* Safe, as long as ISzInStream *s is the first field in SZfileinstream */

    *size = s->io->read(s->io, buffer, *size);

    return SZ_OK;
} /* sz_file_read */


/*
 * Seek implementation, to be passed to 7z
 */
static SRes sz_file_seek(const ISeekInStream *object, Int64 *pos, ESzSeek origin)
{
    BAIL_IF(pos == NULL, PHYSFS_ERR_INVALID_ARGUMENT, SZ_ERROR_PARAM);

    SZfileinstream *s = (SZfileinstream *)object; /* Safe, as long as ISzInStream *s is the first field in SZfileinstream */
    PHYSFS_uint64 position = *pos;

    switch (origin) {
        case SZ_SEEK_SET:
            break;
        case SZ_SEEK_CUR:
            position = s->io->tell(s->io) + position;
            break;
        case SZ_SEEK_END:
            position = s->io->length(s->io) - position;
            break;
    }

    if (!s->io->seek(s->io, position))
        return SZ_ERROR_FAIL;

    *pos = position;

    return SZ_OK;
} /* sz_file_seek */


/*
 * Translate Microsoft FILETIME (used by 7zip) into UNIX timestamp
 */
static PHYSFS_sint64 sz_filetime_to_unix_timestamp(CNtfsFileTime *ft)
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
    return strcmp(name, file->name);
} /* sz_file_cmp_stdlib */


/*
 * Compare two files with each other based on the name
 * Used for sorting
 */
static int sz_file_cmp(void *_a, size_t one, size_t two)
{
    SZfile *files = (SZfile *) _a;
    return strcmp(files[one].name, files[two].name);
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
    SZfile *file = bsearch(name, archive->files, archive->db.NumFiles, sizeof(*archive->files), sz_file_cmp_stdlib);

    BAIL_IF(file == NULL, PHYSFS_ERR_NOT_FOUND, NULL);

    return file;
} /* sz_find_file */


/*
 * Load metadata for the file at given index
 */
static int sz_file_init(SZarchive *archive, PHYSFS_uint32 fileIndex)
{
    SZfile *file = &archive->files[fileIndex];
    PHYSFS_uint32 folderIndex = archive->db.FileToFolder[fileIndex];

    file->index = fileIndex; /* Store index into 7z array, since we sort our own. */
    file->archive = archive;
    file->folder = (folderIndex != (PHYSFS_uint32)-1 ? &archive->folders[folderIndex] : NULL); /* Directories don't have a folder (they contain no own data...) */
    CSzFileItem *n_item = allocator.Malloc(sizeof(CSzFileItem));

    n_item->Size = SzArEx_GetFileSize(&archive->db, fileIndex);
    n_item->IsDir = SzArEx_IsDir(&archive->db, fileIndex);
    n_item->MTime = archive->db.MTime.Vals[fileIndex];
    //n_item->MTimeDefined = archive->db.MTime.Defs[fileIndex];
    if(SzBitWithVals_Check(&archive->db.MTime, fileIndex))
        n_item->MTimeDefined = 1;
    else
        n_item->MTimeDefined = 0;

    file->item = n_item;
    //file->item = &archive->db.db.Files[fileIndex]; /* Holds crucial data and is often referenced -> Store link */
    file->position = 0;
    file->offset = -1; /* Offset will be set by SZ_read() */

    size_t len = SzArEx_GetFileNameUtf16(&file->archive->db, file->index, NULL);

    PHYSFS_uint16 *buf16 = allocator.Malloc(len * sizeof(*buf16));
    SzArEx_GetFileNameUtf16(&file->archive->db, file->index, buf16);

    size_t len8 = len * 2; /* UTF-16 strings may need double the size in UTF-8 */
    char *buf8 = allocator.Malloc(len8);
    PHYSFS_utf8FromUtf16(buf16, buf8, len8);

    file->name = buf8;
    allocator.Free(buf16);

    return 1;
} /* sz_file_init */


/*
 * Load metadata for all files
 */
static int sz_files_init(SZarchive *archive)
{
    PHYSFS_uint32 fileIndex = 0, numFiles = archive->db.NumFiles;

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
    archive->allocImp = &g_Alloc;

    archive->allocTempImp = &g_Alloc;

    archive->inStream.s.Read = sz_file_read;
    archive->inStream.s.Seek = sz_file_seek;

    /* Prepare input wrapper callbacks for 7z */
    LookToRead2_CreateVTable(&archive->lookStream, False);
    archive->lookStream.buf = NULL;
    archive->lookStream.buf = ISzAlloc_Alloc(archive->allocImp, kInputBufSize);
    if (archive->lookStream.buf)
    {
      archive->lookStream.bufSize = kInputBufSize;
      archive->lookStream.realStream = &archive->inStream.s;
      LookToRead2_Init(&archive->lookStream);
    }
} /* sz_archive_init */


/*
 * Deinitialise archive
 */
static void sz_archive_exit(SZarchive *archive)
{
    PHYSFS_uint32 fileIndex = 0, numFiles = archive->db.NumFiles;

    for (fileIndex = 0; fileIndex < numFiles; fileIndex++)
    {
        allocator.Free((char*)archive->files[fileIndex].name);
        allocator.Free(archive->files[fileIndex].item);
    } /* for */

    /* Free arrays */
    allocator.Free(archive->folders);
    allocator.Free(archive->files);
    allocator.Free(archive);
} /* sz_archive_exit */

/*
 * Wrap all 7z calls in this, so the physfs error state is set appropriately.
 */
static int sz_err(SRes rc)
{
    switch (rc)
    {
        case SZ_OK:
            break;
        case SZ_ERROR_DATA:
            PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
            break;
        case SZ_ERROR_MEM:
            PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
            break;
        case SZ_ERROR_CRC:
            PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
            break;
        case SZ_ERROR_UNSUPPORTED:
            PHYSFS_setErrorCode(PHYSFS_ERR_UNSUPPORTED);
            break;
        case SZ_ERROR_PARAM:
            PHYSFS_setErrorCode(PHYSFS_ERR_INVALID_ARGUMENT);
            break;
        case SZ_ERROR_INPUT_EOF:
            PHYSFS_setErrorCode(PHYSFS_ERR_PAST_EOF);
            break;
        case SZ_ERROR_OUTPUT_EOF:
            PHYSFS_setErrorCode(PHYSFS_ERR_PAST_EOF);
            break;
        case SZ_ERROR_READ:
            PHYSFS_setErrorCode(PHYSFS_ERR_IO);
            break;
        case SZ_ERROR_WRITE:
            PHYSFS_setErrorCode(PHYSFS_ERR_IO);
            break;
        case SZ_ERROR_PROGRESS:
            PHYSFS_setErrorCode(PHYSFS_ERR_OTHER_ERROR); /* !!! FIXME: right? */
            break;
        case SZ_ERROR_FAIL:
            PHYSFS_setErrorCode(PHYSFS_ERR_OTHER_ERROR);  /* !!! FIXME: right? */
            break;
        case SZ_ERROR_THREAD:
            PHYSFS_setErrorCode(PHYSFS_ERR_OTHER_ERROR);  /* !!! FIXME: right? */
            break;
        case SZ_ERROR_ARCHIVE:
            PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
            break;
        case SZ_ERROR_NO_ARCHIVE:
            PHYSFS_setErrorCode(PHYSFS_ERR_OTHER_ERROR); /* !!! FIXME: right? */
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

    BAIL_IF_ERRPASS(wantedSize == 0, 0); /* quick rejection. */
    BAIL_IF(remainingSize == 0, PHYSFS_ERR_PAST_EOF, 0);

    if (wantedSize > remainingSize)
        wantedSize = remainingSize;

    /* Only decompress the folder if it is not already cached */
    if (file->folder->cache == NULL)
    {
        size_t fileSize = 0;
        const int rc = sz_err(SzArEx_Extract(
            &file->archive->db, /* 7z's database, containing everything */
            &file->archive->lookStream.vt, /* compressed data */
            file->index, /* Index into database arrays */
            /* Index of cached folder, will be changed by SzExtract */
            &file->folder->index,
            /* Cache for decompressed folder, allocated/freed by SzExtract */
            &file->folder->cache,
            /* Size of cache, will be changed by SzExtract */
            &file->folder->size,
            /* Offset of this file inside the cache, set by SzExtract */
            &file->offset,
            /* Size of this file, set by SzExtract */
            &fileSize,
            file->archive->allocImp,
            file->archive->allocTempImp));

        if (rc != SZ_OK)
            return -1;

        BAIL_IF(wantedSize > fileSize, PHYSFS_ERR_OTHER_ERROR, -1);
        BAIL_IF(file->item->Size != fileSize, PHYSFS_ERR_OTHER_ERROR, -1);
    } /* if */

    /* Copy wanted bytes over from cache to outBuf */
    memcpy(outBuf, (file->folder->cache + file->offset + file->position),
            wantedSize);
    file->position += wantedSize; /* Increase virtual position */

    return wantedSize;
} /* SZ_read */


static PHYSFS_sint64 SZ_write(PHYSFS_Io *io, const void *b, PHYSFS_uint64 len)
{
    BAIL(PHYSFS_ERR_READ_ONLY, -1);
} /* SZ_write */


static PHYSFS_sint64 SZ_tell(PHYSFS_Io *io)
{
    SZfile *file = (SZfile *) io->opaque;
    return file->position;
} /* SZ_tell */


static int SZ_seek(PHYSFS_Io *io, PHYSFS_uint64 offset)
{
    SZfile *file = (SZfile *) io->opaque;

    BAIL_IF(offset > file->item->Size, PHYSFS_ERR_PAST_EOF, 0);

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
    BAIL(PHYSFS_ERR_UNSUPPORTED, NULL);
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

        /* file and folder are static parts of the archive - keep them around */
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


static void *SZ_openArchive(PHYSFS_Io *io, const char *name, int forWriting, int *claimed)
{
    size_t len = 0;
    SZarchive *archive = NULL;

    BAIL_IF(io == NULL, PHYSFS_ERR_INVALID_ARGUMENT, NULL);
    BAIL_IF(forWriting, PHYSFS_ERR_READ_ONLY, NULL);

    archive = (SZarchive *) allocator.Malloc(sizeof (SZarchive));
    BAIL_IF(archive == NULL, PHYSFS_ERR_OUT_OF_MEMORY, NULL);

    sz_archive_init(archive);
    archive->inStream.io = io;

    SzArEx_Init(&archive->db);

    SRes res = SzArEx_Open(&archive->db,
                           &archive->lookStream.vt,
                           archive->allocImp,
                           archive->allocTempImp);

    /* Not a 7z archive */
    if (res == SZ_ERROR_NO_ARCHIVE)
    {
        SzArEx_Free(&archive->db, archive->allocImp);
        sz_archive_exit(archive);
        return NULL;
    }
    else if (sz_err(res) != SZ_OK)
    {
        SzArEx_Free(&archive->db, archive->allocImp);
        sz_archive_exit(archive);
        return NULL; /* Error is set by sz_err! */
    } /* if */
    *claimed = 1;

    len = archive->db.NumFiles * sizeof (SZfile);
    archive->files = (SZfile *) allocator.Malloc(len);
    if (archive->files == NULL)
    {
        SzArEx_Free(&archive->db, archive->allocImp);
        sz_archive_exit(archive);
        BAIL(PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    }

    /*
     * Init with 0 so we know when a folder is already cached
     * Values will be set by SZ_openRead()
     */
    memset(archive->files, 0, len);

    len = archive->db.db.NumFolders * sizeof (SZfolder);
    archive->folders = (SZfolder *) allocator.Malloc(len);
    if (archive->folders == NULL)
    {
        SzArEx_Free(&archive->db, archive->allocImp);
        sz_archive_exit(archive);
        BAIL(PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    }

    /*
     * Init with 0 so we know when a folder is already cached
     * Values will be set by SZ_read()
     */
    memset(archive->folders, 0, len);

    if(!sz_files_init(archive))
    {
        SzArEx_Free(&archive->db, archive->allocImp);
        sz_archive_exit(archive);
        BAIL(PHYSFS_ERR_OTHER_ERROR, NULL);
    }

    return archive;
} /* SZ_openArchive */


/*
 * Moved to seperate function so we can use alloca then immediately throw
 *  away the allocated stack space...
 */
static PHYSFS_EnumerateCallbackResult doEnumCallback(PHYSFS_EnumerateCallback cb, void *callbackdata,
                           const char *odir, const char *str, size_t flen)
{
    char *newstr = __PHYSFS_smallAlloc(flen + 1);
    if (newstr == NULL)
        return PHYSFS_ENUM_ERROR;

    memcpy(newstr, str, flen);
    newstr[flen] = '\0';
    PHYSFS_EnumerateCallbackResult retval = cb(callbackdata, odir, newstr);
    __PHYSFS_smallFree(newstr);
    return retval;
} /* doEnumCallback */


static PHYSFS_EnumerateCallbackResult SZ_enumerateFiles(void *opaque,
                         const char *dname, PHYSFS_EnumerateCallback cb,
                         const char *origdir, void *callbackdata)
{
    size_t dlen = strlen(dname),
           dlen_inc = dlen + ((dlen > 0) ? 1 : 0);
    SZarchive *archive = (SZarchive *) opaque;
    SZfile *file = NULL,
           *lastFile = &archive->files[archive->db.NumFiles];

    PHYSFS_EnumerateCallbackResult retval = PHYSFS_ENUM_OK;

    if (dlen)
    {
        file = sz_find_file(archive, dname);
        if (file != NULL) /* if 'file' is NULL it should stay so, otherwise errors will not be handled */
            file += 1; /* Currently pointing to the directory itself, skip it */
    }
    else
    {
        file = archive->files;
    }

    BAIL_IF(file == NULL, PHYSFS_ERR_NOT_FOUND, PHYSFS_ENUM_ERROR);

    while (file < lastFile)
    {
        const char * fname = file->name;
        const char * dirNameEnd = fname + dlen_inc;

        if (strncmp(dname, fname, dlen) != 0) /* Stop after mismatch, archive->files is sorted */
            break;

        if (strchr(dirNameEnd, '/')) /* Skip subdirs */
        {
            file++;
            continue;
        }

        /* Do the actual callback... */
        retval = doEnumCallback(cb, callbackdata, origdir, dirNameEnd, strlen(dirNameEnd));
        if (retval == PHYSFS_ENUM_ERROR)
            PHYSFS_setErrorCode(PHYSFS_ERR_APP_CALLBACK);
        file++;
    }
    return retval;
} /* SZ_enumerateFiles */


static PHYSFS_Io *SZ_openRead(void *opaque, const char *path)
{
    SZarchive *archive = (SZarchive *) opaque;
    SZfile *file = sz_find_file(archive, path);
    PHYSFS_Io *io = NULL;

    BAIL_IF(file == NULL, PHYSFS_ERR_NOT_FOUND, NULL);
    BAIL_IF(file->folder == NULL, PHYSFS_ERR_NOT_A_FILE, NULL);

    file->position = 0;
    file->folder->references++; /* Increase refcount for automatic cleanup... */

    io = (PHYSFS_Io *) allocator.Malloc(sizeof (PHYSFS_Io));
    BAIL_IF(io == NULL, PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    memcpy(io, &SZ_Io, sizeof (*io));
    io->opaque = file;

    return io;
} /* SZ_openRead */


static PHYSFS_Io *SZ_openWrite(void *opaque, const char *filename)
{
    BAIL(PHYSFS_ERR_READ_ONLY, NULL);
} /* SZ_openWrite */


static PHYSFS_Io *SZ_openAppend(void *opaque, const char *filename)
{
    BAIL(PHYSFS_ERR_READ_ONLY, NULL);
} /* SZ_openAppend */


static void SZ_closeArchive(void *opaque)
{
    SZarchive *archive = (SZarchive *) opaque;

    //We need to do this cleanup first because SzArEx_Free drops NumFiles to 0
    // and if we do the sz_archive_exit before SzArEx_Free then SzArEx_Free crashes

    PHYSFS_uint32 fileIndex = 0, numFiles = archive->db.NumFiles;

    for (fileIndex = 0; fileIndex < numFiles; fileIndex++)
    {
        allocator.Free((char*)archive->files[fileIndex].name);
        allocator.Free(archive->files[fileIndex].item);
    }

    SzArEx_Free(&archive->db, archive->allocImp);
    archive->inStream.io->destroy(archive->inStream.io);
    sz_archive_exit(archive);
} /* SZ_closeArchive */


static int SZ_remove(void *opaque, const char *name)
{
    BAIL(PHYSFS_ERR_READ_ONLY, 0);
} /* SZ_remove */


static int SZ_mkdir(void *opaque, const char *name)
{
    BAIL(PHYSFS_ERR_READ_ONLY, 0);
} /* SZ_mkdir */

static int SZ_stat(void *opaque, const char *path, PHYSFS_Stat *stat)
{
    const SZarchive *archive = (const SZarchive *) opaque;
    const SZfile *file = sz_find_file(archive, path);

    if (!file)
        return 0;

    if(file->item->IsDir)
    {
        stat->filesize = 0;
        stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
    } /* if */
    else
    {
        stat->filesize = (PHYSFS_sint64) file->item->Size;
        stat->filetype = PHYSFS_FILETYPE_REGULAR;
    } /* else */

    if (file->item->MTimeDefined)
        stat->modtime = sz_filetime_to_unix_timestamp(&file->item->MTime);
    else
        stat->modtime = -1;

    /* real create and accesstype are currently not in the lzma SDK */
    stat->createtime = stat->modtime;
    stat->accesstime = stat->modtime;

    stat->readonly = 1;  /* 7zips are always read only */

    return 1;
} /* SZ_stat */

void SZIP_global_init(void)
{
    /* this just needs to calculate some things, so it only ever
       has to run once, even after a deinit. */
    static int generatedTable = 0;
    if (!generatedTable)
    {
        generatedTable = 1;
        CrcGenerateTable();
    } /* if */
} /* SZIP_global_init */


const PHYSFS_Archiver __PHYSFS_Archiver_7Z =
{
    CURRENT_PHYSFS_ARCHIVER_API_VERSION,
    {
        "7Z",
        "7zip archives",
        "Dennis Schridde <devurandom@gmx.net>",
        "https://icculus.org/physfs/",
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
