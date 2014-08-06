#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "sqlcipher.h"

#ifdef SQLCIPHER_VFS_DEBUG
#define SQLCIPHER_VFS_TRACE(X)  {printf X;fflush(stdout);}
#else
#define SQLCIPHER_VFS_TRACE(X)
#endif


static const unsigned char sqlcipherMagic[] = {
  0xB0, 0x08, 0xA6, 0x79, 0x75, 0x7E, 0x3E, 0x9E, 
  0xF3, 0x00, 0x58, 0xDD, 0xB8, 0x9D, 0xE2, 0x3B, 
  0x7D, 0x92, 0xDA, 0xAF, 0xE0, 0x11, 0x0A, 0x5F, 
  0x05, 0x76, 0x4A, 0xF6, 0xED, 0x9D, 0xE4, 0x84
};

/* these four sqlite3_file functions account for header allocation */
static int sqlcipherVfsRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int sqlcipherVfsWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64);
static int sqlcipherVfsTruncate(sqlite3_file*, sqlite3_int64 size);
static int sqlcipherVfsFileSize(sqlite3_file*, sqlite3_int64 *pSize);

/* remaining sqlite3_file functions are transparent passthroughs */
static int sqlcipherVfsClose(sqlite3_file*);
static int sqlcipherVfsSync(sqlite3_file*, int flags);
static int sqlcipherVfsLock(sqlite3_file*, int);
static int sqlcipherVfsUnlock(sqlite3_file*, int);
static int sqlcipherVfsCheckReservedLock(sqlite3_file*, int *);
static int sqlcipherVfsFileControl(sqlite3_file*, int op, void *pArg);
static int sqlcipherVfsSectorSize(sqlite3_file*);
static int sqlcipherVfsDeviceCharacteristics(sqlite3_file*);
static int sqlcipherVfsShmLock(sqlite3_file*,int,int,int);
static int sqlcipherVfsShmMap(sqlite3_file*,int,int,int, void volatile **);
static void sqlcipherVfsShmBarrier(sqlite3_file*);
static int sqlcipherVfsShmUnmap(sqlite3_file*,int);

static int sqlcipherVfsOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);

#define SQLCIPHER_MAGIC_SZ 32
#define SQLCIPHER_MIN_HDR_SZ (SQLCIPHER_MAGIC_SZ+sizeof(u32))

/* 
 * Header format:
 * Offset    Size      Description
 * 0         32        File Magic Header
 * 32        4         reserve_sz: Header Size (should be a power of 2 and sector size)
 * 36        4         version: SQLCipher Header Version Number
 * 40        4         page_sz: Database Page Size
 * 44        4         kdf_iter: KDF Iterations
 * 48        4         fast_kdf_iter: Fast KDF Iterations
 * 52        4         flags: Flags (i.e. CIPHER_FLAG_HMAC)
 */

static int sqlcipherVfsReadHeader(sqlcipherVfs_file *file) {
  sqlite_int64 fsize;
  unsigned char magic[SQLCIPHER_MIN_HDR_SZ];
  unsigned char *header;

  file->use_header = file->needs_write = file->did_read = 0;

  if(file->pReal->pMethods->xRead(file->pReal, magic, SQLCIPHER_MIN_HDR_SZ, 0) == SQLITE_OK) {
    SQLCIPHER_VFS_TRACE(("peak at first 36 bytes from file header\n"));
    /* file exists and read first 36s, which will include the magic and the full size of the header
     * then compare the first 32 bytes to the magic;
     */
    if(memcmp(magic, sqlcipherMagic, SQLCIPHER_MAGIC_SZ) == 0) {
      SQLCIPHER_VFS_TRACE(("file header magic matches setting reserve size to 32\n"));
      file->reserve_sz = sqlite3Get4byte(&magic[32]);

      header = sqlcipher_malloc(file->reserve_sz);
      if(file->pReal->pMethods->xRead(file->pReal, header, file->reserve_sz, 0) == SQLITE_OK) {
        file->version = sqlite3Get4byte(header+36);
        file->page_sz = sqlite3Get4byte(header+40);
        file->kdf_iter = sqlite3Get4byte(header+44);
        file->fast_kdf_iter = sqlite3Get4byte(header+48);
        file->flags = sqlite3Get4byte(header+52);

        SQLCIPHER_VFS_TRACE(("unpacked file header use_header=%u, reserve_sz=%u, version=%u, \
                              page_sz=%u, kdf_iter=%u, fast_kdf_iter=%u, flags=%u\n",
                              file->use_header, file->reserve_sz, file->version, file->page_sz, file->kdf_iter, 
                              file->fast_kdf_iter, file->flags));

        file->use_header = file->did_read = 1;
      } else {
        SQLCIPHER_VFS_TRACE(("error reading full header header\n"));
      }
      sqlcipher_free(header, file->reserve_sz);

    } else {
      SQLCIPHER_VFS_TRACE(("file header does not match magic setting reserve size to 0\n"));
      file->reserve_sz = 0;
    }
  } else if (file->pReal->pMethods->xFileSize(file->pReal, &fsize) == SQLITE_OK && fsize == 0) {
    SQLCIPHER_VFS_TRACE(("file size is 0, database doesnt exist, setting reserve size\n"));
    file->reserve_sz = file->pReal->pMethods->xSectorSize(file->pReal); /* header should be aligned to sector */
    file->needs_write = 1;
  } else {
    SQLCIPHER_VFS_TRACE(("unknown issue\n"));
  }
  
  return SQLITE_OK;
}

static int sqlcipherVfsWriteHeader(sqlcipherVfs_file *file) {
  unsigned char *header = sqlcipher_malloc(file->reserve_sz); 
  
  memcpy(header, sqlcipherMagic, SQLCIPHER_MAGIC_SZ);
  sqlite3Put4byte(header+32, file->reserve_sz);
  sqlite3Put4byte(header+36, file->version);
  sqlite3Put4byte(header+40, file->page_sz);
  sqlite3Put4byte(header+44, file->kdf_iter);
  sqlite3Put4byte(header+48, file->fast_kdf_iter);
  sqlite3Put4byte(header+52, file->flags);

  SQLCIPHER_VFS_TRACE(("packed file header use_header=%u, reserve_sz=%u, version=%u, \
                        page_sz=%u, kdf_iter=%u, fast_kdf_iter=%u, flags=%u\n",
                        file->use_header, file->reserve_sz, file->version, file->page_sz, file->kdf_iter, 
                        file->fast_kdf_iter, file->flags));

  if(file->pReal->pMethods->xWrite(file->pReal, header, file->reserve_sz, 0) == SQLITE_OK) {
    SQLCIPHER_VFS_TRACE(("wrote file header\n"));
  } else {
    SQLCIPHER_VFS_TRACE(("file header write failed!\n"));
  }
  
  sqlcipher_free(header, file->reserve_sz);

  return SQLITE_OK;
}


static int sqlcipherVfsRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  SQLCIPHER_VFS_TRACE(("sqlcipherVfsRead path=%s, iAmt=%d, iOfst=%lld\n", p->filename, iAmt, iOfst));
  return p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst + (p->use_header ? p->reserve_sz : 0));
}

static int sqlcipherVfsWrite(
  sqlite3_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  SQLCIPHER_VFS_TRACE(("sqlcipherVfsWrite path=%s, iAmt=%d, iOfst=%lld\n", p->filename, iAmt, iOfst));
  if(p->use_header && p->reserve_sz >= SQLCIPHER_MIN_HDR_SZ && iOfst == 0 && p->needs_write) {
    sqlcipherVfsWriteHeader(p);
    p->needs_write = 0;
  } 
  return p->pReal->pMethods->xWrite(p->pReal, zBuf, iAmt, iOfst + (p->use_header ? p->reserve_sz : 0));
}

static int sqlcipherVfsTruncate(sqlite3_file *pFile, sqlite_int64 size){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  SQLCIPHER_VFS_TRACE(("sqlcipherVfsWrite path=%s, size=%lld\n", p->filename, size));
  return p->pReal->pMethods->xTruncate(p->pReal, size + (p->use_header ? p->reserve_sz : 0));
}

static int sqlcipherVfsFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  int rc;
  sqlite_int64 rSize;
  rc = p->pReal->pMethods->xFileSize(p->pReal, &rSize);
  if(p->use_header) {
    /* return file size minus reserve, but floor at zero */
    *pSize = (rSize >= p->reserve_sz) ? rSize - p->reserve_sz : 0; 
  } else {
    *pSize = rSize;
  }

  SQLCIPHER_VFS_TRACE(("sqlcipherVfsWrite path=%s, rSize=%lld, pSize=%lld\n", p->filename, rSize, *pSize));
  return rc;
}

static int sqlcipherVfsClose(sqlite3_file *pFile){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xClose(p->pReal);
}

static int sqlcipherVfsSync(sqlite3_file *pFile, int flags){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xSync(p->pReal, flags);
}

static int sqlcipherVfsLock(sqlite3_file *pFile, int eLock){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xLock(p->pReal, eLock);
}

static int sqlcipherVfsUnlock(sqlite3_file *pFile, int eLock){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xUnlock(p->pReal, eLock);
}

static int sqlcipherVfsCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
}

static int sqlcipherVfsFileControl(sqlite3_file *pFile, int op, void *pArg){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
}

static int sqlcipherVfsSectorSize(sqlite3_file *pFile){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xSectorSize(p->pReal);
}

static int sqlcipherVfsDeviceCharacteristics(sqlite3_file *pFile){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
}

static int sqlcipherVfsShmLock(sqlite3_file *pFile, int ofst, int n, int flags){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xShmLock(p->pReal, ofst, n, flags);
}

static int sqlcipherVfsShmMap(
  sqlite3_file *pFile,
  int iRegion,
  int szRegion,
  int isWrite,
  void volatile **pp
){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xShmMap(p->pReal, iRegion, szRegion, isWrite, pp);
}

static void sqlcipherVfsShmBarrier(sqlite3_file *pFile){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}

static int sqlcipherVfsShmUnmap(sqlite3_file *pFile, int delFlag){
  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  return p->pReal->pMethods->xShmUnmap(p->pReal, delFlag);
}


static int sqlcipherVfsOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  sqlite_int64 fsize;

  SQLCIPHER_VFS_TRACE(("sqlcipherVfsOpen\n"));

  sqlcipherVfs_file *p = (sqlcipherVfs_file *)pFile;
  sqlcipherVfs_info *pInfo = (sqlcipherVfs_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  p->pInfo = pInfo;
  p->pReal = (sqlite3_file *)&p[1];
  rc = pRoot->xOpen(pRoot, zName, p->pReal, flags, pOutFlags);

  p->filename = zName;

  /* setup some file methods for sqlcipher vfs override */
  if( p->pReal->pMethods ){
    sqlite3_io_methods *pNew = sqlite3_malloc( sizeof(*pNew) );
    const sqlite3_io_methods *pSub = p->pReal->pMethods;
    memset(pNew, 0, sizeof(*pNew));

    pNew->iVersion = pSub->iVersion;
    pNew->xClose = sqlcipherVfsClose;
    pNew->xRead = sqlcipherVfsRead; /* modified to account for header */
    pNew->xWrite = sqlcipherVfsWrite; /* modified to account for header */
    pNew->xTruncate = sqlcipherVfsTruncate; /* modified to account for header */
    pNew->xSync = sqlcipherVfsSync;
    pNew->xFileSize = sqlcipherVfsFileSize; /* modified to account for header */
    pNew->xLock = sqlcipherVfsLock;
    pNew->xUnlock = sqlcipherVfsUnlock;
    pNew->xCheckReservedLock = sqlcipherVfsCheckReservedLock;
    pNew->xFileControl = sqlcipherVfsFileControl;
    pNew->xSectorSize = sqlcipherVfsSectorSize;
    pNew->xDeviceCharacteristics = sqlcipherVfsDeviceCharacteristics;
    if( pNew->iVersion>=2 ){
      pNew->xShmMap = pSub->xShmMap ? sqlcipherVfsShmMap : 0;
      pNew->xShmLock = pSub->xShmLock ? sqlcipherVfsShmLock : 0;
      pNew->xShmBarrier = pSub->xShmBarrier ? sqlcipherVfsShmBarrier : 0;
      pNew->xShmUnmap = pSub->xShmUnmap ? sqlcipherVfsShmUnmap : 0;
    }

    pFile->pMethods = pNew;
  }

  sqlcipherVfsReadHeader(p);
  return rc;
}

/*
** Register SQLCipher VFS shim
**
** Return SQLITE_OK on success.  
**
** SQLITE_NOMEM is returned in the case of a memory allocation error.
** SQLITE_NOTFOUND is returned if zOldVfsName does not exist.
*/
int sqlcipherVfs_register(
   const char *zOldVfsName          /* Name of the underlying VFS */
){
  sqlite3_vfs *pNew;
  sqlite3_vfs *pRoot;
  sqlcipherVfs_info *pInfo;
  const char *newVfsName = "sqlcipher";
  int nName;
  int nByte;

  SQLCIPHER_VFS_TRACE(("sqlcipherVfs_register\n"));

  pNew = sqlite3_vfs_find(newVfsName);
  if( pNew == NULL) {
    SQLCIPHER_VFS_TRACE(("sqlcipher has not yet been registered\n"));
    pRoot = sqlite3_vfs_find(zOldVfsName);
    if( pRoot==0 ) return SQLITE_NOTFOUND;
    nName = strlen(newVfsName);
    nByte = sizeof(*pNew) + sizeof(*pInfo) + nName + 1;
    pNew = sqlite3_malloc( nByte );
    if( pNew==0 ) return SQLITE_NOMEM;
    memset(pNew, 0, nByte);
    pInfo = (sqlcipherVfs_info*)&pNew[1];
  
    pNew->szOsFile = pRoot->szOsFile + sizeof(sqlcipherVfs_file);
    pNew->zName = (char*)&pInfo[1];
    memcpy((char*)&pInfo[1], newVfsName, nName+1);
    pNew->pAppData = pInfo;
  
    /* override xOpen so we use our own custom 
     * sqlcipherVfs_file implementation */
    pNew->xOpen = sqlcipherVfsOpen;
  
    /* all other VFS functions will point directly back to root */
    pNew->iVersion = pRoot->iVersion;
    pNew->mxPathname = pRoot->mxPathname;
    pNew->xDelete = pRoot->xDelete;
    pNew->xFullPathname = pRoot->xFullPathname;
    pNew->xAccess = pRoot->xAccess;
    pNew->xDlOpen = pRoot->xDlOpen;
    pNew->xDlError = pRoot->xDlError;
    pNew->xDlSym = pRoot->xDlSym;
    pNew->xDlClose = pRoot->xDlClose;
    pNew->xRandomness = pRoot->xRandomness;
    pNew->xSleep = pRoot->xSleep;
    pNew->xCurrentTime = pRoot->xCurrentTime;
    pNew->xGetLastError = pRoot->xGetLastError;
  
    if( pNew->iVersion>=2 ){
      pNew->xCurrentTimeInt64 = pRoot->xCurrentTimeInt64;
  
      if( pNew->iVersion>=3 ){
        pNew->xSetSystemCall = pRoot->xSetSystemCall;
        pNew->xGetSystemCall = pRoot->xGetSystemCall;
        pNew->xNextSystemCall = pRoot->xNextSystemCall;
      }
    }
  
    pInfo->pRootVfs = pRoot;
    pInfo->pSqlcipherVfs = pNew;
  } else {
    // if sqlcipher has already been registered, just ensure it's default
    SQLCIPHER_VFS_TRACE(("sqlcipher VFS has already been registered, skipping registration\n"));
  }

  return sqlite3_vfs_register(pNew, 1);
}
