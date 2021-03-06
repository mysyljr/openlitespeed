/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2015  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include <shm/lsshm.h>

#include <shm/lsshmpool.h>
#include <util/gpath.h>

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>


extern "C" {
    int ls_expandfile(int fd, LsShmOffset_t fromsize, LsShmXSize_t incrsize);
};


LsShmVersion LsShm::s_version =
{
    { LSSHM_VER_MAJOR, LSSHM_VER_MINOR, LSSHM_VER_REL, LSSHM_VER_TYPE }
};
LsShmSize_t LsShm::s_iPageSize = LSSHM_PAGESIZE;
LsShmSize_t LsShm::s_iShmHdrSize = ((sizeof(LsShmMap) + 0xf) &
                                    ~0xf); // align 16
const char *LsShm::s_pDirBase = NULL;
LsShmStatus_t LsShm::s_errStat = LSSHM_OK;
int LsShm::s_iErrNo = 0;
char LsShm::s_aErrMsg[LSSHM_MAXERRMSG] = { 0 };


//
//  house keeping stuff
//
// LsShm *LsShm::s_pBase = NULL;
static HashStringMap< LsShm * > *s_pBase = NULL;


static inline HashStringMap< LsShm * > *getBase()
{
    if (s_pBase == NULL)
        s_pBase = new HashStringMap< LsShm * >();
    return s_pBase;
}


//
//  @brief setDir - allow configuration to set the path for SHM data
//
LsShmStatus_t LsShm::setSysShmDir(const char *dirName)
{
    if ((dirName != NULL) && (*dirName != '\0'))
    {
        struct stat mystat;
        if ((stat(dirName, &mystat) == 0)
            && (S_ISDIR(mystat.st_mode) || S_ISLNK(mystat.st_mode)))
        {
            char *newP;
            if ((newP = strdup(dirName)) != NULL)
            {
                if (s_pDirBase != NULL)
                    free((void *)(s_pDirBase));
                s_pDirBase = newP;
                return LSSHM_OK;
            }
        }
    }
    return LSSHM_BADPARAM;
}


//
//  @brief setErrMsg - set the global static error message
//
LsShmStatus_t LsShm::setErrMsg(LsShmStatus_t stat, const char *fmt, ...)
{
    int ret;
    va_list va;

    s_errStat = stat;
    s_iErrNo = errno;
    va_start(va, fmt);
    ret = vsnprintf(s_aErrMsg, LSSHM_MAXERRMSG, fmt, va);
    va_end(va);
    if (ret >= LSSHM_MAXERRMSG)
        strcpy(&s_aErrMsg[LSSHM_MAXERRMSG - 4], "...");
    return LSSHM_OK;
}


LsShm::LsShm()
    : m_locks()
    , m_iMagic(LSSHM_MAGIC)
    , m_iMaxShmSize(0)
    , m_status(LSSHM_NOTREADY)
    , m_pFileName(NULL)
    , m_pMapName(NULL)
    , m_iFd(-1)
    , m_pShmLock(NULL)
    , m_pRegLock(NULL)
    , x_pShmMap(NULL)
    , m_pShmMapO(NULL)
    , m_iMaxSizeO(0)
    , m_pGPool(NULL)
    , m_iRef(0)
{

#ifdef DELAY_UNMAP
    m_uCur = m_uArray;
    m_uEnd = m_uCur;
#endif

#if 0
// destructor will clean this up
    else
        cleanup();
#endif
}


LsShm::~LsShm()
{
    if (m_pFileName != NULL)
    {
#ifdef DEBUG_RUN
        SHM_NOTICE("LsShm::~LsShm remove %s <%p>", m_pFileName, s_pBase);
#endif
        getBase()->remove(m_pFileName);
    }
    cleanup();
}


const char *LsShm::getDefaultShmDir()
{
    static int isDirTected = 0;
    if (isDirTected == 0)
    {
        const char *pathname = "/dev/shm";
        struct stat sb;
        isDirTected =
            ((stat(pathname, &sb) == 0) && S_ISDIR(sb.st_mode)) ? 1 : 2;
    }

    return (isDirTected == 1) ? LSSHM_SYSSHM_DIR1 : LSSHM_SYSSHM_DIR2;
}


static int lockFile(int fd, short lockType = F_WRLCK)
{
    int ret;
    struct flock lock;
    lock.l_type = lockType;
    lock.l_start = 0;
    lock.l_whence = SEEK_SET;
    lock.l_len = 0;
    while (1)
    {
        ret = fcntl(fd, F_SETLKW, &lock);
        if ((ret == -1) && (errno == EINTR))
            continue;
        return ret;
    }
}


LsShm *LsShm::open(const char *mapName, LsShmXSize_t initSize,
                   const char *pBaseDir)
{
    LsShm *pObj;
    if (mapName == NULL)
        return NULL;

    char buf[0x1000];
    GHash::iterator itor;
    snprintf(buf, sizeof(buf), "%s/%s.%s",
             (pBaseDir != NULL) ? pBaseDir : getDefaultShmDir(),
             mapName, LSSHM_SYSSHM_FILE_EXT);

    itor = getBase()->find(buf);
#ifdef DEBUG_RUN
    SHM_NOTICE("LsShm::get find %s <%p>", buf, s_pBase);
#endif
    if (itor != NULL)
    {
#ifdef DEBUG_RUN
        SHM_NOTICE("LsShm::get find %s <%p> return <%p>",
                   buf, s_pBase, itor);
#endif
        pObj = (LsShm *)itor->second();
        pObj->upRef();
        return pObj;
    }

    LsShmStatus_t stat;
    pObj = new LsShm();
    if (pObj != NULL)
    {
        stat = pObj->initShm(mapName, initSize, pBaseDir);
        if (pObj->m_iFd != -1)
            lockFile(pObj->m_iFd, F_UNLCK);
        if (stat == LSSHM_OK)
            return pObj;

        if (stat == LSSHM_BADVERSION)
            pObj->deleteFile();

//         SHM_NOTICE(
//             "ERROR: FAILED TO CREATE SHARED MEMORY %d MAPNAME [%s] DIR [%s] ",
//             pObj->status(), mapName,
//             (pBaseDir ? pBaseDir : getDefaultShmDir()));
        delete pObj;
    }
    else
        stat = LSSHM_SYSERROR;
    if (*getErrMsg() == '\0')       // if no error message, set generic one
        setErrMsg(stat, "Unable to setup SHM MapFile [%s].", buf);

    return NULL;
}


// LsShm *LsShm::get(const char *mapName, int initsize)
// {
//     LsShm *pObj;
//     if (mapName == NULL)
//        return NULL;
//
//     GHash::iterator itor;
// #ifdef DEBUG_RUN
//     SHM_NOTICE("LsShm::get find %s <%p>", mapName, &s_base);
// #endif
//     itor = s_base.find(mapName);
//     if (itor != NULL)
//     {
// #ifdef DEBUG_RUN
//         SHM_NOTICE("LsShm::get find %s <%p> return <%p>",
//           mapName, &s_base, itor);
// #endif
//         pObj = (LsShm *)itor->second();
//         pObj->upRef();
//         return pObj;
//     }
//
//     pObj = new LsShm(mapName, initsize);
//     if (pObj != NULL)
//     {
//         if (pObj->status() == LSSHM_READY)
//             return pObj;
//
//         SHM_NOTICE(
//           "ERROR: FAILED TO CREATE SHARE MEMORY %d MAPNAME [%s] DIR [%s] ",
//           pObj->status(), mapName,
//           (s_pDirBase? s_pDirBase: getDefaultShmDir()));
//         delete pObj;
//     }
//     return NULL;
// }


LsShmStatus_t LsShm::checkMagic(LsShmMap *mp, const char *mName) const
{
    return ((mp->x_iMagic != m_iMagic)
            || (mp->x_version.m_iVer != s_version.m_iVer)
            || (strncmp((char *)mp->x_aName, mName, LSSHM_MAXNAMELEN) != 0)) ?
           LSSHM_BADMAPFILE : LSSHM_OK;
}


void LsShm::deleteFile()
{
    if (m_pFileName != NULL)
        unlink(m_pFileName);
}


void LsShm::cleanup()
{
    unmap();
    if (m_iFd != -1)
    {
        ::close(m_iFd);
        m_iFd = -1;
    }
    if (m_pFileName != NULL)
    {
        free(m_pFileName);
        m_pFileName = NULL;
    }
    if (m_pMapName != NULL)
    {
        free(m_pMapName);
        m_pMapName = NULL;
    }
}


//
// expand the file to a particular size
//
LsShmStatus_t LsShm::expandFile(LsShmOffset_t from, LsShmXSize_t incrSize)
{
    if (m_iMaxShmSize && ((from + incrSize) > m_iMaxShmSize))
        return LSSHM_BADMAXSPACE;

    if ((from + incrSize) < from)   // wrapped
    {
        setErrMsg(LSSHM_BADMAXSPACE,
                  "Unable to expand [%s], Exceeded internal data size [%lld].",
                  m_pFileName, (uint64_t)from + (uint64_t)incrSize);
        return LSSHM_BADMAXSPACE;
    }

    if (ls_expandfile(m_iFd, from, incrSize) < 0)
    {
        setErrMsg(LSSHM_BADNOSPACE, "Unable to expand [%s], incr=%lu, %s.",
                  m_pFileName, (unsigned long)incrSize, strerror(errno));
        return LSSHM_BADNOSPACE;
    }
    return LSSHM_OK;
}


LsShmStatus_t LsShm::openLockShmFile()
{

    if ((m_iFd = ::open(m_pFileName, O_RDWR | O_CREAT, 0750)) < 0)
    {
        if ((GPath::createMissingPath(m_pFileName, 0750) < 0)
            || ((m_iFd = ::open(m_pFileName, O_RDWR | O_CREAT, 0750)) < 0))
        {
            setErrMsg(LSSHM_SYSERROR, "Unable to open/create [%s], %s.",
                      m_pFileName, strerror(errno));
            return LSSHM_BADMAPFILE;
        }
    }

    ::fcntl(m_iFd, F_SETFD, FD_CLOEXEC);
    lockFile(m_iFd);
    return LSSHM_OK;
}


LsShmStatus_t LsShm::initShm(const char *mapName, LsShmXSize_t size,
                             const char *pBaseDir)
{
    LsShmReg       *p_reg;
    struct stat     mystat;
    char            buf[0x1000];
    struct timeval  tv;
    uint64_t        id = 0;

    // m_status = LSSHM_NOTREADY;

    if ((mapName == NULL) || (strlen(mapName) >= LSSHM_MAXNAMELEN)
        || (m_pMapName = strdup(mapName)) == NULL)
    {
        m_status = LSSHM_BADPARAM;
        return m_status;
    }

    snprintf(buf, sizeof(buf), "%s/%s.%s",
             (pBaseDir != NULL) ? pBaseDir : getDefaultShmDir(),
             mapName, LSSHM_SYSSHM_FILE_EXT);
    if ((m_pFileName = strdup(buf)) == NULL)
    {
        m_status = LSSHM_ERROR;
        return m_status;
    }

    if (openLockShmFile() != LSSHM_OK)
        return LSSHM_BADMAPFILE;

    size = ((size + s_iPageSize - 1) / s_iPageSize) * s_iPageSize;

    if (fstat(m_iFd, &mystat) < 0)
    {
        setErrMsg(LSSHM_SYSERROR, "Unable to stat [%s], %s.",
                  m_pFileName, strerror(errno));
        return LSSHM_BADMAPFILE;
    }
    snprintf(buf, sizeof(buf), "%s/%s.%s",
             (pBaseDir != NULL) ? pBaseDir : getDefaultShmDir(),
             mapName, LSSHM_SYSLOCK_FILE_EXT);

    if (mystat.st_size == 0)
    {
        gettimeofday(&tv, NULL);
        id = tv.tv_sec * 1000000 + tv.tv_usec;
        unlink(buf);
    }

    int fdLock = ::open(buf, O_RDWR | O_CREAT, 0750);
    if (fdLock == -1
        || (m_status = m_locks.init(buf, fdLock, LSSHM_MINLOCK, id)) != LSSHM_OK)
    {
        if (*getErrMsg() == '\0')       // if no error message, set generic one
            setErrMsg(LSSHM_BADMAPFILE, "Unable to set SHM LockFile for [%s].", buf);
        return m_status;
    }
    ::fcntl(fdLock, F_SETFD, FD_CLOEXEC);

    if (mystat.st_size == 0)
    {
        // New File!!!
        //  creating a new map
        if (size < s_iPageSize)
            size = s_iPageSize;
        if ((expandFile(0, roundToPageSize(size)) != LSSHM_OK)
            || (map(size) != LSSHM_OK))
            return LSSHM_ERROR;
        x_pShmMap->x_iMagic = m_iMagic;
        x_pShmMap->x_version.m_iVer = s_version.m_iVer;
        x_pShmMap->x_id     = id;
        x_pShmMap->x_iFreeOffset = 0;     // no free list yet
        x_pShmMap->x_iRegBlkOffset = 0;
        x_pShmMap->x_iRegLastBlkOffset = 0;
        x_pShmMap->x_iMaxRegNum = 0;
        x_pShmMap->x_stat.m_iFileSize = size;               // x_iMaxSize
        x_pShmMap->x_stat.m_iUsedSize = LSSHM_SHM_UNITSIZE; // x_iCurSize
        x_pShmMap->x_stat.m_iAllocated = 1;   // first block for self
        x_pShmMap->x_stat.m_iReleased = 0;
        x_pShmMap->x_stat.m_iFreeListCnt = 0;

        x_pShmMap->x_iLockOffset[0] = 0;
        x_pShmMap->x_iLockOffset[1] = 0;
        if (((m_pShmLock = allocLock()) == NULL)
            || ((m_pRegLock = allocLock()) == NULL)
            || (setupLocks() != LSSHM_OK))
            return LSSHM_ERROR;
        x_pShmMap->x_iLockOffset[0] = pLock2offset(m_pShmLock);
        x_pShmMap->x_iLockOffset[1] = pLock2offset(m_pRegLock);

        strncpy((char *)x_pShmMap->x_aName, m_pMapName, strlen(m_pMapName));

        if ((LsShm::allocRegBlk(NULL) == NULL)
            || ((p_reg = addReg(m_pMapName)) == NULL))
            return LSSHM_ERROR;
        p_reg->x_iFlag = LSSHM_REGFLAG_SET;
        assert(p_reg->x_iRegNum == 0);
    }
    else
    {
        // Old File
        if (mystat.st_size < s_iShmHdrSize)
        {
            setErrMsg(LSSHM_BADMAPFILE, "Bad SHM file format [%s], size=%lld.",
                      m_pFileName, (uint64_t)mystat.st_size);
            return LSSHM_BADMAPFILE;
        }

        // only map the header size to ensure the file is good.
        if (map((LsShmXSize_t)s_iShmHdrSize) != LSSHM_OK)
            return LSSHM_ERROR;

        if (checkMagic(x_pShmMap, m_pMapName) != LSSHM_OK)
        {
            setErrMsg(LSSHM_BADVERSION,
                      "Bad SHM file format [%s], size=%lld, magic=%08X(%08X).",
                      m_pFileName, (uint64_t)mystat.st_size,
                      x_pShmMap->x_iMagic, m_iMagic);
            return LSSHM_BADVERSION;
        }

        if (x_pShmMap->x_id != m_locks.getId())
        {
            setErrMsg(LSSHM_BADVERSION,
                      "SHM file [%s] ID=%lld, LOCK file ID=%lld.",
                      m_pFileName, x_pShmMap->x_id, m_locks.getId());
            return LSSHM_BADVERSION;
        }

        // expand the file if needed... won't shrink
        if (size > x_pShmMap->x_stat.m_iFileSize)
        {
            if (expandFile((LsShmOffset_t)x_pShmMap->x_stat.m_iFileSize,
                           (LsShmXSize_t)(size - x_pShmMap->x_stat.m_iFileSize)) != LSSHM_OK)
                return LSSHM_ERROR;
            x_pShmMap->x_stat.m_iFileSize = size;
        }
        else
            size = mystat.st_size;

        unmap();
        if (map(size) != LSSHM_OK)
            return LSSHM_ERROR;

        m_pShmLock = offset2pLock(x_pShmMap->x_iLockOffset[0]);
        m_pRegLock = offset2pLock(x_pShmMap->x_iLockOffset[1]);
    }

    getGlobalPool();
    m_status = LSSHM_READY;
    m_iRef = 1;
    getBase()->insert(m_pFileName, this);

    return LSSHM_OK;
}


// will not shrink at the current moment
LsShmStatus_t LsShm::expand(LsShmXSize_t incrSize)
{
    LsShmXSize_t xsize = x_pShmMap->x_stat.m_iFileSize;

    if (expandFile((LsShmOffset_t)xsize, incrSize) != LSSHM_OK)
        return LSSHM_ERROR;

#ifdef DELAY_UNMAP
    unmapLater();
#else
    unmap();
#endif
    xsize += incrSize;
    if (map(xsize) != LSSHM_OK)
    {
        xsize -= incrSize;
        if (xsize != 0)
            map(xsize);     // try to map old size for cleanup
        return LSSHM_ERROR;
    }
    x_pShmMap->x_stat.m_iFileSize = xsize;

    return LSSHM_OK;
}


LsShmStatus_t LsShm::map(LsShmXSize_t size)
{
    uint8_t *p =
        (uint8_t *)mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, m_iFd,
                        0);
    if (p == MAP_FAILED)
    {
        setErrMsg(LSSHM_SYSERROR, "Unable to mmap [%s], size=%lu, %s.",
                  m_pFileName, (unsigned long)size, strerror(errno));
        return LSSHM_SYSERROR;
    }

    x_pShmMap = (LsShmMap *)p;
    m_iMaxSizeO = size;
    return LSSHM_OK;
}


LsShmStatus_t LsShm::remap()
{
#ifdef DEBUG_RUN
    SHM_NOTICE("LsShm::remap %6d %X %X %X",
               getpid(), x_pShmMap, x_pShmMap->x_stat.m_iFileSize, m_iMaxSizeO);
#endif

    LsShmXSize_t size = x_pShmMap->x_stat.m_iFileSize;
#ifdef DELAY_UNMAP
    unmapLater();
#else
    unmap();
#endif
    return map(size);
}


void LsShm::unmap()
{
    if (x_pShmMap != NULL)
    {
        m_pShmMapO = x_pShmMap;
        munmap(x_pShmMap, m_iMaxSizeO);
        x_pShmMap = NULL;
        m_iMaxSizeO = 0;
    }
}


LsShmOffset_t LsShm::getFromFreeList(LsShmSize_t size)
{
    LsShmOffset_t offset;
    LShmFreeTop *ap;

    offset = x_pShmMap->x_iFreeOffset;
    while (offset != 0)
    {
        ap = (LShmFreeTop *)offset2ptr(offset);
        if (ap->x_iFreeSize >= size)
        {
            LsShmSize_t left = ap->x_iFreeSize - size;
            reduceFreeFromBot(ap, offset, left);
            return offset + left;
        }
        offset = ap->x_iFreeNext;
    }
    return 0; // no match
}


//
//  reduceFreeFromBot - reduce to size or unlink myself from FreeList
//
void LsShm::reduceFreeFromBot(
    LShmFreeTop *ap, LsShmOffset_t offset, LsShmSize_t newsize)
{
    if (newsize == 0)
    {
        // remove myself from freelist
        markTopUsed(ap);
        disconnectFromFree(ap, (LShmFreeBot *)offset2ptr(ap->x_iFreeSize - sizeof(
                               LShmFreeBot)));
        return;
    }

    //
    // NOTE: reduce size from bottom
    //
    LShmFreeBot *bp =
        (LShmFreeBot *)offset2ptr(offset + newsize - sizeof(LShmFreeBot));

    bp->x_iFreeOffset = offset;
    bp->x_iBMarker = LSSHM_FREE_BMARKER;
    ap->x_iFreeSize = newsize;
}


void LsShm::disconnectFromFree(LShmFreeTop *ap, LShmFreeBot *bp)
{
    LsShmOffset_t myNext, myPrev;
    myNext = ap->x_iFreeNext;
    myPrev = ap->x_iFreePrev;
    LShmFreeTop *xp;

    if (myPrev != 0)
    {
        xp = (LShmFreeTop *)offset2ptr(myPrev);
        xp->x_iFreeNext = myNext;
    }
    else
        x_pShmMap->x_iFreeOffset = myNext;
    if (myNext != 0)
    {
        xp = (LShmFreeTop *)offset2ptr(myNext);
        xp->x_iFreePrev = myPrev;
    }
    --x_pShmMap->x_stat.m_iFreeListCnt;
}


//
//  @brief isFreeBlockAbove - is the space above this block "freespace".
//  @return true if the block above is free otherwise false
//
bool LsShm::isFreeBlockAbove(
    LsShmOffset_t offset, LsShmSize_t size, int joinFlag)
{
    LsShmSize_t aboveOffset = offset - sizeof(LShmFreeBot);
    if (aboveOffset < LSSHM_SHM_UNITSIZE)
        return false;

    LShmFreeBot *bp = (LShmFreeBot *)offset2ptr(aboveOffset);
    if ((bp->x_iBMarker == LSSHM_FREE_BMARKER)
        && (bp->x_iFreeOffset >= LSSHM_SHM_UNITSIZE)
        && (bp->x_iFreeOffset < aboveOffset))
    {
        LShmFreeTop *ap = (LShmFreeTop *)offset2ptr(bp->x_iFreeOffset);
        if ((ap->x_iAMarker == LSSHM_FREE_AMARKER)
            && ((ap->x_iFreeSize + bp->x_iFreeOffset) == offset))
        {
            if (joinFlag != 0)
            {
                // join above block
                ap->x_iFreeSize += size;

                LShmFreeBot *xp =
                    (LShmFreeBot *)offset2ptr(offset + size - sizeof(LShmFreeBot));
                xp->x_iBMarker = LSSHM_FREE_BMARKER;
                xp->x_iFreeOffset = bp->x_iFreeOffset;

                markTopUsed((LShmFreeTop *)offset2ptr(offset));
            }
            return true;
        }
    }
    return false;
}


//
// Check if this is a good free block
//
bool LsShm::isFreeBlockBelow(
    LsShmOffset_t offset, LsShmSize_t size, int joinFlag)
{
    LsShmOffset_t belowOffset = offset + size;

    LShmFreeTop *ap = (LShmFreeTop *)offset2ptr(belowOffset);
    if (ap->x_iAMarker == LSSHM_FREE_AMARKER)
    {
        // the bottom free offset
        LsShmOffset_t e_offset = belowOffset + ap->x_iFreeSize;
        if ((e_offset < LSSHM_SHM_UNITSIZE)
            || (e_offset > x_pShmMap->x_stat.m_iFileSize))
            return false;

        e_offset -= sizeof(LShmFreeBot);
        LShmFreeBot *bp = (LShmFreeBot *)offset2ptr(e_offset);
        if ((bp->x_iBMarker == LSSHM_FREE_BMARKER)
            && (bp->x_iFreeOffset == belowOffset))
        {
            if (joinFlag != 0)
            {
                markTopUsed(ap);
                if (joinFlag == 2)
                {
                    disconnectFromFree(ap, bp);
                    // merge to top
                    LShmFreeTop *xp = (LShmFreeTop *)offset2ptr(offset);
                    xp->x_iFreeSize += ap->x_iFreeSize;
                    bp->x_iFreeOffset = offset;
                    return true;
                }

                // setup myself as free block
                LShmFreeTop *np = (LShmFreeTop *)offset2ptr(offset);
                np->x_iAMarker = LSSHM_FREE_AMARKER;
                np->x_iFreeSize = size + ap->x_iFreeSize;
                np->x_iFreeNext = ap->x_iFreeNext;
                np->x_iFreePrev = ap->x_iFreePrev;

                bp->x_iFreeOffset = offset;

                LShmFreeTop *xp;
                if (np->x_iFreeNext != 0)
                {
                    xp = (LShmFreeTop *)offset2ptr(np->x_iFreeNext);
                    xp->x_iFreePrev = offset;
                }
                if (np->x_iFreePrev != 0)
                {
                    xp = (LShmFreeTop *)offset2ptr(np->x_iFreePrev);
                    xp->x_iFreeNext = offset;
                }
                else
                    x_pShmMap->x_iFreeOffset = offset;
            }
            return true;
        }
    }
    return false;
}


//
//  Simple bucket type allocation and release
//
LsShmOffset_t LsShm::allocPage(LsShmSize_t pagesize, int &remap)
{
    LsShmOffset_t offset;
    LsShmSize_t availSize;

    LSSHM_CHECKSIZE(pagesize);

    pagesize = roundUnitSize(pagesize);
    remap = 0;

    //
    //  MUTEX SHOULD BE HERE for multi process/thread environment
    // Only use lock when m_status is Ready.
    if (m_status == LSSHM_READY)
        if (lock() > 0)
            return 0; // no lock acquired...

    // Allocate from free space
    if ((offset = getFromFreeList(pagesize)) != 0)
        goto out;

    // Allocate from heap space
    availSize = avail();
    if (pagesize > availSize)
    {
        LsShmXSize_t needSize;
        // min 16 unit at a time
        needSize = ((pagesize - availSize) > (16 * LSSHM_SHM_UNITSIZE)) ?
                   (pagesize - availSize) : (16 * LSSHM_SHM_UNITSIZE);
        if (expand(roundToPageSize(needSize)) != LSSHM_OK)
        {
            offset = 0;
            goto out;
        }
        if (x_pShmMap != m_pShmMapO)
            remap = 1;
    }
    offset = x_pShmMap->x_stat.m_iUsedSize;
    markTopUsed((LShmFreeTop *)offset2ptr(offset));
    used(pagesize);
out:
    if (offset != 0)
        incrCheck(&x_pShmMap->x_stat.m_iAllocated,
                  (pagesize / LSSHM_SHM_UNITSIZE));
    if (m_status == LSSHM_READY)
        unlock();
    return offset;
}


void LsShm::releasePage(LsShmOffset_t offset, LsShmSize_t pagesize)
{
    pagesize = roundUnitSize(pagesize);
    //
    //  MUTEX SHOULD BE HERE for multi process/thread environment
    //
    lock();

    incrCheck(&x_pShmMap->x_stat.m_iReleased, (pagesize / LSSHM_SHM_UNITSIZE));
    if ((offset + pagesize) == x_pShmMap->x_stat.m_iUsedSize)
    {
        x_pShmMap->x_stat.m_iUsedSize -= pagesize;
        goto out;
    }
    if (isFreeBlockAbove(offset, pagesize, 1))
    {
        LShmFreeTop *ap;
        offset = ((LShmFreeBot *)offset2ptr(
                      offset + pagesize - sizeof(LShmFreeBot)))->x_iFreeOffset;
        ap = (LShmFreeTop *)offset2ptr(offset);
        isFreeBlockBelow(offset, ap->x_iFreeSize, 2);
        goto out;
    }
    if (isFreeBlockBelow(offset, pagesize, 1))
        goto out;
    joinFreeList(offset, pagesize);
out:
    unlock();
}


void LsShm::joinFreeList(LsShmOffset_t offset, LsShmSize_t size)
{
    // setup myself as free block
    LShmFreeTop *np = (LShmFreeTop *)offset2ptr(offset);
    np->x_iAMarker = LSSHM_FREE_AMARKER;
    np->x_iFreeSize = size;
    np->x_iFreeNext = x_pShmMap->x_iFreeOffset;
    np->x_iFreePrev = 0;

    // join myself to freeList
    x_pShmMap->x_iFreeOffset = offset;

    if (np->x_iFreeNext != 0)
    {
        LShmFreeTop *xp = (LShmFreeTop *)offset2ptr(np->x_iFreeNext);
        xp->x_iFreePrev = offset;
    }

    LShmFreeBot *bp =
        (LShmFreeBot *)offset2ptr(offset + size - sizeof(LShmFreeBot));
    bp->x_iFreeOffset = offset;
    bp->x_iBMarker = LSSHM_FREE_BMARKER;
    ++x_pShmMap->x_stat.m_iFreeListCnt;
}


//
//  SHM Reg sub interface
//
LsShmRegBlk *LsShm::allocRegBlk(LsShmRegBlkHdr *pFrom)
{
    LsShmOffset_t offset;
    LsShmRegBlkHdr *p_regBlkHdr;

    int remap;
    offset = allocPage(sizeof(LsShmRegElem) * LSSHM_REGPERBLK, remap);
    if (offset == 0)
        return NULL;
    p_regBlkHdr = (LsShmRegBlkHdr *)offset2ptr(offset);
    p_regBlkHdr->x_iCapacity = LSSHM_REGPERBLK;
    p_regBlkHdr->x_iSize = 1;
    p_regBlkHdr->x_iNext = 0;

    if (pFrom != NULL)
    {
        p_regBlkHdr->x_iStartNum = pFrom->x_iStartNum + pFrom->x_iCapacity - 1;
        pFrom->x_iNext = offset;
    }
    else
    {
        x_pShmMap->x_iRegBlkOffset = offset;
        p_regBlkHdr->x_iStartNum = 0;
    }

    x_pShmMap->x_iRegLastBlkOffset = offset;
    x_pShmMap->x_iMaxRegNum += (LSSHM_REGPERBLK - 1);

    // initialize the block data
    LsShmReg *p_reg = (LsShmReg *)(((LsShmRegElem *)p_regBlkHdr) + 1);
    int regnum = p_regBlkHdr->x_iStartNum;
    int i = p_regBlkHdr->x_iCapacity;
    while (--i > 0)
    {
        p_reg->x_iRegNum = regnum;
        ++p_reg;
        ++regnum;
    }

    // note LsShmRegBlk == LsShmRegBlkHdr == LsShmReg[0]
    return (LsShmRegBlk *)p_regBlkHdr;
}


LsShmRegBlk *LsShm::findRegBlk(LsShmRegBlkHdr *pFrom, int num)
{
    if ((LsShmOffset_t)num >= pFrom->x_iStartNum)
    {
        if ((LsShmOffset_t)num < (pFrom->x_iStartNum + pFrom->x_iCapacity - 1))
            return (LsShmRegBlk *)pFrom;
        if (pFrom->x_iNext != 0)
            return findRegBlk((LsShmRegBlkHdr *)offset2ptr(pFrom->x_iNext), num);
    }
    return NULL;
}


LsShmReg *LsShm::getReg(int num)
{
    LsShmRegBlkHdr *pFrom;
    LsShmOffset_t regoff = x_pShmMap->x_iRegBlkOffset;
    if ((regoff != 0)
        && (pFrom = (LsShmRegBlkHdr *)
                    findRegBlk((LsShmRegBlkHdr *)offset2ptr(regoff), num)) != NULL)
    {
        return (LsShmReg *)
               (((LsShmRegElem *)pFrom) + 1 + num - pFrom->x_iStartNum);
    }
    return NULL;
}


int LsShm::clrReg(int num)
{
    lockReg();
    LsShmRegBlkHdr *pFrom;
    LsShmOffset_t regoff = x_pShmMap->x_iRegBlkOffset;
    if ((regoff != 0)
        && (pFrom = (LsShmRegBlkHdr *)
                    findRegBlk((LsShmRegBlkHdr *)offset2ptr(regoff), num)) != NULL)
    {
        LsShmRegElem *p_regElem =
            ((LsShmRegElem *)pFrom) + 1 + num - pFrom->x_iStartNum;
        if (p_regElem->x_reg.x_iFlag != LSSHM_REGFLAG_CLR)
        {
            p_regElem->x_reg.x_iFlag = LSSHM_REGFLAG_CLR; // clear it!
            --pFrom->x_iSize;
            unlockReg();
            return 0;
        }
    }
    unlockReg();
    return LS_FAIL;
}


void LsShm::expandReg()
{
    LsShmCnt_t maxRegNum = x_pShmMap->x_iMaxRegNum + LSSHM_REGPERBLK - 1;
    LsShmRegBlkHdr *p_regBlkHdr = NULL;

    p_regBlkHdr = (LsShmRegBlkHdr *)((x_pShmMap->x_iRegLastBlkOffset == 0) ?
                                     allocRegBlk(0) : offset2ptr(x_pShmMap->x_iRegLastBlkOffset));

    while ((p_regBlkHdr != NULL)
           && (x_pShmMap->x_iMaxRegNum <= (LsShmOffset_t)maxRegNum))
        p_regBlkHdr = (LsShmRegBlkHdr *)allocRegBlk(p_regBlkHdr);

    return;
}


// currently, names in the registry are always null terminated,
// so the max number of chars is LSSHM_MAXNAMELEN-1
LsShmReg *LsShm::findReg(const char *name)
{
    int nameSize;
    if ((name == NULL) || (*name == '\0')
        || (x_pShmMap->x_iRegBlkOffset == 0)
        || ((nameSize = strlen(name) + 1) > LSSHM_MAXNAMELEN))  // including null
        return NULL;

    LsShmRegBlkHdr *p_regBlkHdr;
    p_regBlkHdr = (LsShmRegBlkHdr *)offset2ptr(x_pShmMap->x_iRegBlkOffset);
    while (p_regBlkHdr != NULL)
    {
        // search the block!
        LsShmRegElem *p_regElem = (LsShmRegElem *)p_regBlkHdr;
        int i = p_regBlkHdr->x_iCapacity;
        while (--i > 0)
        {
            ++p_regElem; // skip the first one
            if ((p_regElem->x_reg.x_iFlag != LSSHM_REGFLAG_CLR)
                && (memcmp(name, p_regElem->x_reg.x_aName, nameSize) == 0))
                return &p_regElem->x_reg;
        }
        if (p_regBlkHdr->x_iNext != 0)
            p_regBlkHdr = (LsShmRegBlkHdr *)offset2ptr(p_regBlkHdr->x_iNext);
        else
            return NULL;
    }
    return NULL;
}


//
// @brief - addReg add the given name into Registry.
// @brief   return the ptr of the registry
//
// currently, names in the registry are always null terminated,
// so the max number of chars is LSSHM_MAXNAMELEN-1
LsShmReg *LsShm::addReg(const char *name)
{
    int nameSize;
    if ((name == NULL) || (*name == '\0')
        || (x_pShmMap->x_iRegBlkOffset == 0)
        || ((nameSize = strlen(name) + 1) > LSSHM_MAXNAMELEN))  // including null
        return NULL;

    lockReg();
    LsShmReg *p_reg;
    if ((p_reg = findReg(name)) != NULL)
    {
        unlockReg();
        return p_reg;
    }

    LsShmRegBlkHdr *p_regBlkHdr;
    p_regBlkHdr = (LsShmRegBlkHdr *)offset2ptr(x_pShmMap->x_iRegBlkOffset);
    LsShmRegElem *p_regElem;
    while (p_regBlkHdr != NULL)
    {
        if (p_regBlkHdr->x_iSize < p_regBlkHdr->x_iCapacity)
        {
            // find the empty slot
            p_regElem = ((LsShmRegElem *)p_regBlkHdr) + 1;
            int i = p_regBlkHdr->x_iCapacity;
            while (--i > 0)
            {
                if (p_regElem->x_reg.x_iFlag == LSSHM_REGFLAG_CLR)
                {
                    memcpy((char *)p_regElem->x_reg.x_aName, name, nameSize);
                    p_regElem->x_reg.x_iFlag = LSSHM_REGFLAG_SET; // assign it!
                    p_regElem->x_reg.x_iValue = 0;
                    ++p_regBlkHdr->x_iSize;
                    unlockReg();
                    return (LsShmReg *)p_regElem;
                }
                ++p_regElem;
            }
        }
        else
        {
            if (p_regBlkHdr->x_iNext != 0)
                p_regBlkHdr = (LsShmRegBlkHdr *)offset2ptr(p_regBlkHdr->x_iNext);
            else
            {
                expandReg();
                p_regBlkHdr = (LsShmRegBlkHdr *)((p_regBlkHdr->x_iNext != 0) ?
                                                 offset2ptr(p_regBlkHdr->x_iNext) : NULL);
            }
        }
    }
    unlockReg();
    return NULL;
}


int LsShm::recoverOrphanShm()
{
    if (getGlobalPool() == NULL)
        return 0;
    LsShmRegBlkHdr *pRegBlkHdr;
    LsShmOffset_t offRegBlkHdr = x_pShmMap->x_iRegBlkOffset;
    int cnt = 0;
    while (offRegBlkHdr != 0)
    {
        pRegBlkHdr = (LsShmRegBlkHdr *)offset2ptr(offRegBlkHdr);
        chkRegBlk(pRegBlkHdr, &cnt);
        offRegBlkHdr = pRegBlkHdr->x_iNext;
    }
    return cnt;
}


void LsShm::chkRegBlk(LsShmRegBlkHdr *pRegBlkHdr, int *pCnt)
{
    uint16_t num = pRegBlkHdr->x_iCapacity;
    LsShmRegElem *pRegElem = (((LsShmRegElem *)pRegBlkHdr) + 1);
    while (--num > 0)
    {
        if (pRegElem->x_reg.x_iFlag & LSSHM_REGFLAG_PID)
        {
            LsShmPoolMem *pPool =
                (LsShmPoolMem *)offset2ptr(pRegElem->x_reg.x_iValue);
            if ((kill(pPool->x_pid, 0) < 0) && (errno == ESRCH))
            {
                m_pGPool->addFreeBucket(&pPool->x_data);
                m_pGPool->addFreeList(&pPool->x_data);
                lockReg();
                pRegElem->x_reg.x_iFlag = LSSHM_REGFLAG_CLR; // clear it!
                --pRegBlkHdr->x_iSize;
                uint32_t offset = pRegElem->x_reg.x_iValue;
                unlockReg();
                m_pGPool->release2(offset, sizeof(LsShmPool));
                ++(*pCnt);
            }
        }
        ++pRegElem;
    }
    return;
}


LsShmPool *LsShm::getGlobalPool()
{
    if ((m_pGPool != NULL) && (m_pGPool->getMagic() == LSSHM_POOL_MAGIC))
        return m_pGPool;

    const char *name = LSSHM_SYSPOOL;
    m_pGPool = new LsShmPool(this, name, NULL);
    if (m_pGPool != NULL)
    {
        if (m_pGPool->getRef() != 0)
            return m_pGPool;

        delete m_pGPool;
        m_pGPool = NULL;
    }
    if (*getErrMsg() == '\0')       // if no error message, set generic one
    {
        setErrMsg(LSSHM_ERROR, "Unable to get Global Pool, MapFile [%s].",
                  m_pFileName);
    }
    return NULL;
}


LsShmPool *LsShm::getNamedPool(const char *name)
{
    LsShmPool *pObj;
    GHash::iterator iter;

    if (name == NULL)
        return getGlobalPool();

#ifdef DEBUG_RUN
    SHM_NOTICE("LsShm::getNamedPool find %s <%p>",
               name, &getObjBase());
#endif
    iter = getObjBase().find(name);
    if (iter != NULL)
    {
#ifdef DEBUG_RUN
        SHM_NOTICE("LsShm::getNamedPool %s <%p> return %p ",
                   name, &getObjBase(), iter);
#endif
        pObj = (LsShmPool *)iter->second();
        if (pObj->getMagic() != LSSHM_POOL_MAGIC)
            return NULL;
        pObj->upRef();
        return pObj;
    }
    LsShmPool *gpool;
    if ((gpool = getGlobalPool()) == NULL)
        return NULL;
    pObj = new LsShmPool(this, name, gpool);
    if (pObj != NULL)
    {
        if (pObj->getRef() != 0)
            return pObj;
        delete pObj;
    }
    if (*getErrMsg() == '\0')       // if no error message, set generic one
    {
        setErrMsg(LSSHM_ERROR, "Unable to get SHM Pool [%s], MapFile [%s].",
                  name, m_pFileName);
    }
    return NULL;
}
